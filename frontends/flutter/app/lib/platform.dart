// Which kind of device this is. The same app runs on phones and on desktop
// computers; a few things only make sense on one of them.
import 'dart:io';

/// A phone or tablet: it has a camera to scan codes with, and an operating
/// system that stops apps it cannot see unless they run a background service.
bool get isPhone => Platform.isAndroid || Platform.isIOS;

/// A desktop computer: a window with a mouse and keyboard, which stays
/// connected for as long as it is open.
bool get isDesktop => Platform.isLinux || Platform.isWindows || Platform.isMacOS;
