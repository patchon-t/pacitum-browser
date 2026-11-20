#pragma once
// 2.0.10 tests that look at pixels, not only at windows:
//  - colorCount(): how many distinct colours an image has (a page that drew nothing has one or two). The
//    smoke test uses it on the web view, so a window that opens but draws no page fails (Wayland without
//    OpenGL passed the old smoke test: the window was there, the page area was empty).
//  - run(): --page-test opens real pages one after another and checks that each loads, gets a title and
//    draws. Pacitum's own pages and a built-in test page must pass; well-known sites must too, except the
//    ones marked "soft" (busy sites that may answer a CI machine with a consent or bot page).
#include <QImage>
#include <QPixmap>

class BrowserWindow;

namespace PageTest {
int colorCount(const QImage& img);   // on a small copy of the image: fast, and noise from scaling stays low
inline int colorCount(const QPixmap& pm) { return pm.isNull() ? 0 : colorCount(pm.toImage()); }
void run(BrowserWindow* w);          // exits the app: 0 = every required page passed, 5 = one failed
}
