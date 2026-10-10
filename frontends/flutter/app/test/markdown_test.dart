import 'package:corded_app/screens/linked_text.dart';
import 'package:flutter_test/flutter_test.dart';

String show(List<StyledRun> runs) => runs
    .map((r) =>
        '${r.bold ? 'B' : ''}${r.italic ? 'I' : ''}${r.strike ? 'S' : ''}${r.code ? 'C' : ''}${r.spoiler ? 'X' : ''}[${r.text}]')
    .join();

void main() {
  test('the common marks become styles', () {
    expect(show(styleRuns('a **b** c')), '[a ]B[b][ c]');
    expect(show(styleRuns('*lean* and _lean_')), 'I[lean][ and ]I[lean]');
    expect(show(styleRuns('~~gone~~')), 'S[gone]');
    expect(show(styleRuns('run `ls -la` now')), '[run ]C[ls -la][ now]');
    expect(show(styleRuns('||secret||')), 'X[secret]');
  });

  test('styles nest, but nothing is styled inside code', () {
    expect(show(styleRuns('**bold and _both_**')), 'B[bold and ]BI[both]');
    expect(show(styleRuns('~~*gone and leaning*~~')), 'IS[gone and leaning]');
    expect(show(styleRuns('`**not bold**`')), 'C[**not bold**]');
  });

  test('marks that are not styling are left alone', () {
    expect(show(styleRuns('2 * 3 * 4')), '[2 * 3 * 4]');
    expect(show(styleRuns('snake_case_name')), '[snake_case_name]');
    expect(show(styleRuns('a ** b')), '[a ** b]');
    expect(show(styleRuns('plain text')), '[plain text]');
  });

  test('quotes and fenced code are their own blocks', () {
    final blocks =
        splitBlocks('hello\n> quoted line\n> and more\nafter\n```\ncode here\n  indented\n```\nend');
    expect(blocks.map((b) => b.kind), ['text', 'quote', 'text', 'code', 'text']);
    expect(blocks[1].text, 'quoted line\nand more');
    expect(blocks[3].text, 'code here\n  indented');
    expect(blocks.last.text, 'end');
  });

  test('headings and bulleted lists', () {
    final blocks = splitBlocks('# Title\n## Smaller\n### Smallest\nwords\n- one\n- two\n* three\nafter');
    expect(blocks.map((b) => b.kind), ['h1', 'h2', 'h3', 'text', 'list', 'text']);
    expect(blocks[0].text, 'Title');
    expect(blocks[4].text, 'one\ntwo\nthree');
    // A # without a space is a channel link or a tag, not a heading; 2 * 3 is not a list.
    expect(splitBlocks('#general is busy').single.kind, 'text');
    expect(splitBlocks('#### too deep').single.kind, 'text');
    expect(splitBlocks('2 * 3').single.kind, 'text');
  });

  test('a message without any of it is one block of text', () {
    final blocks = splitBlocks('just words');
    expect(blocks.single.kind, 'text');
    expect(blocks.single.text, 'just words');
  });
}
