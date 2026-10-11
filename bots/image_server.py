#!/usr/bin/env python3
"""A small picture server for the image bot: Stable Diffusion on this machine.

    python image_server.py [--model stabilityai/stable-diffusion-xl-base-1.0] [--port 7860]

Unlike the bots, this needs more than the standard library: PyTorch and
Hugging Face's `diffusers`, in a Python environment of their own (the README
says how). It listens on this machine only and answers the one request the
image bot makes, in the shape the AUTOMATIC1111 web UI made common
(`POST /sdapi/v1/txt2img`), so the bot can as well be pointed at Forge,
SD.Next or AUTOMATIC1111 if you run one of those instead.

The model is loaded at the first request, not at start, and stays on the
graphics card while pictures are being asked for. After --idle seconds
without a request (45 unless changed) it is unloaded altogether, so the card
and the memory are free for other things: the chatbot's model, a game. The
next picture after that takes some seconds longer, for the loading. An SDXL
model wants about 7 GB of an 8 GB card while it draws.
"""
import argparse
import base64
import io
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

parser = argparse.ArgumentParser()
parser.add_argument("--model", default="stabilityai/stable-diffusion-xl-base-1.0",
                    help="a model's name on Hugging Face, or a folder holding one")
parser.add_argument("--vae", default="madebyollin/sdxl-vae-fp16-fix",
                    help="the part that turns the drawing into a picture, in a form that works in half precision; "
                         "'' to use the model's own (for models that are not SDXL)")
parser.add_argument("--host", default="127.0.0.1")
parser.add_argument("--port", type=int, default=7860)
parser.add_argument("--ollama", default="http://127.0.0.1:11434",
                    help="an Ollama on the same graphics card, asked to unload before the model loads ('' for none)")
parser.add_argument("--idle", type=int, default=45,
                    help="seconds without a request after which the model is unloaded from the graphics card")
args = parser.parse_args()

one_at_a_time = threading.Lock()
state = {"pipe": None, "step": 0, "steps": 0, "busy": False, "used": 0.0}


def free_the_card():
    """Asks an Ollama on this machine to set down the chat model it holds:
    that one and a picture model do not fit an 8 GB card together."""
    if not args.ollama:
        return
    import urllib.request
    try:
        with urllib.request.urlopen(args.ollama.rstrip("/") + "/api/ps", timeout=5) as answer:
            loaded = [m.get("name") for m in json.load(answer).get("models", [])]
        for name in loaded:
            request = urllib.request.Request(args.ollama.rstrip("/") + "/api/generate",
                                             data=json.dumps({"model": name, "keep_alive": 0}).encode(),
                                             headers={"Content-Type": "application/json"})
            urllib.request.urlopen(request, timeout=30).read()
            print(f"asked Ollama to unload {name}", flush=True)
        if loaded:
            time.sleep(2)   # the card is given back a moment after the answer
    except Exception:  # noqa: BLE001  (no Ollama here: nothing to free)
        pass


def pipeline():
    """The model, loaded if it is not. Its largest part goes straight to the
    graphics card and never sits in ordinary memory; the two parts that turn
    words into numbers are small and wait in ordinary memory between uses."""
    if state["pipe"] is None:
        import torch
        from diffusers import AutoPipelineForText2Image, UNet2DConditionModel
        free_the_card()
        print(f"loading {args.model} ...", flush=True)
        began = time.time()
        unet = UNet2DConditionModel.from_pretrained(args.model, subfolder="unet", torch_dtype=torch.float16,
                                                    variant="fp16", use_safetensors=True).to("cuda")
        pipe = AutoPipelineForText2Image.from_pretrained(args.model, unet=unet, torch_dtype=torch.float16,
                                                         variant="fp16", use_safetensors=True)
        if args.vae:
            # The model's own last stage has to work in full precision, which an
            # 8 GB card has no room for beside the rest; this one does not.
            from diffusers import AutoencoderKL
            pipe.vae = AutoencoderKL.from_pretrained(args.vae, torch_dtype=torch.float16, use_safetensors=True)
        pipe.vae.to("cuda")
        pipe.vae.enable_tiling()
        pipe.vae.enable_slicing()
        pipe.set_progress_bar_config(disable=True)
        state["pipe"] = pipe
        print(f"loaded in {time.time() - began:.0f}s", flush=True)
    return state["pipe"]


def unload():
    """Lets go of the model altogether: the graphics card and the memory are
    free again for whatever else runs here."""
    import gc

    import torch
    with one_at_a_time:
        if state["pipe"] is None or time.time() - state["used"] < args.idle:
            return
        state["pipe"] = None
        gc.collect()
        torch.cuda.empty_cache()
        print("idle: the model is unloaded", flush=True)


def watch_idle():
    while True:
        time.sleep(5)
        if state["pipe"] is not None and not state["busy"] and time.time() - state["used"] >= args.idle:
            unload()


def make(request):
    """One picture. If the graphics card turns out to be full (something else
    took it meanwhile), everything is set down, the card is asked for, and
    the picture is tried once more."""
    import gc

    import torch
    try:
        return draw(request)
    except torch.OutOfMemoryError:
        print("the graphics card was full; freeing it and trying once more", flush=True)
        with one_at_a_time:
            state["pipe"] = None
            gc.collect()
            torch.cuda.empty_cache()
        return draw(request)


def draw(request):
    import torch
    prompt = str(request.get("prompt", ""))[:2000]
    negative = str(request.get("negative_prompt", ""))[:2000]
    width = max(256, min(1536, int(request.get("width", 1024)) // 8 * 8))
    height = max(256, min(1536, int(request.get("height", 1024)) // 8 * 8))
    steps = max(1, min(60, int(request.get("steps", 25))))
    seed = int(request.get("seed", -1))
    if seed < 0:
        seed = int.from_bytes(os.urandom(4), "big")
    with one_at_a_time:
        state.update(step=0, steps=steps, busy=True)
        try:
            pipe = pipeline()
            # The words are read on the graphics card, then their readers step aside for the drawing.
            encoders = [e for e in (pipe.text_encoder, getattr(pipe, "text_encoder_2", None)) if e is not None]
            for encoder in encoders:
                encoder.to("cuda")
            with torch.no_grad():
                read = pipe.encode_prompt(prompt=prompt, negative_prompt=negative, device="cuda",
                                          do_classifier_free_guidance=True)
            for encoder in encoders:
                encoder.to("cpu")
            torch.cuda.empty_cache()
            names = ("prompt_embeds", "negative_prompt_embeds", "pooled_prompt_embeds", "negative_pooled_prompt_embeds")
            words = dict(zip(names, read))

            def progress(_pipe, step, _timestep, kwargs):
                state["step"] = step + 1
                return kwargs
            latents = pipe(**{k: v for k, v in words.items() if v is not None}, width=width, height=height,
                           num_inference_steps=steps, guidance_scale=float(request.get("cfg_scale", 7.0)),
                           generator=torch.Generator("cpu").manual_seed(seed),
                           callback_on_step_end=progress, output_type="latent").images
            # The last stage, done apart so that the drawing's working memory is given back first.
            torch.cuda.empty_cache()
            with torch.no_grad():
                decoded = pipe.vae.decode(latents.to(pipe.vae.dtype) / pipe.vae.config.scaling_factor).sample
            image = pipe.image_processor.postprocess(decoded, output_type="pil")[0]
            del latents, decoded
        finally:
            state.update(busy=False, used=time.time())
            torch.cuda.empty_cache()
    out = io.BytesIO()
    image.save(out, format="PNG")
    return {"images": [base64.b64encode(out.getvalue()).decode()],
            "info": json.dumps({"seed": seed, "width": width, "height": height, "steps": steps})}


class Handler(BaseHTTPRequestHandler):
    def answer(self, code, body):
        raw = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path.startswith("/sdapi/v1/progress"):
            done = state["step"] / state["steps"] if state["busy"] and state["steps"] else 0.0
            self.answer(200, {"progress": done, "state": {"sampling_step": state["step"], "sampling_steps": state["steps"]}})
        else:
            self.answer(200, {"model": args.model, "loaded": state["pipe"] is not None, "busy": state["busy"]})

    def do_POST(self):
        if self.path.startswith("/unload"):
            # Another program on this machine wants the graphics card: given, unless a picture is being made.
            if state["busy"]:
                self.answer(200, {"unloaded": False, "busy": True})
            else:
                state["used"] = 0.0
                unload()
                self.answer(200, {"unloaded": True})
            return
        if not self.path.startswith("/sdapi/v1/txt2img"):
            self.answer(404, {"error": "only /sdapi/v1/txt2img is served here"})
            return
        try:
            request = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            began = time.time()
            result = make(request)
            print(f"a picture in {time.time() - began:.0f}s", flush=True)
            self.answer(200, result)
        except Exception as error:  # noqa: BLE001
            print(f"failed: {error}", flush=True)
            self.answer(500, {"error": str(error)})

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    threading.Thread(target=watch_idle, daemon=True).start()
    print(f"picture server on http://{args.host}:{args.port}, model {args.model} (loaded at the first request)", flush=True)
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
