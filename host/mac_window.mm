// macOS window tweaks the SDL2 API does not expose: per-pixel transparency so
// the device floats on the desktop rather than in a grey box, and click-through
// so the invisible parts of the window do not swallow clicks meant for whatever
// is behind it.

#import <Cocoa/Cocoa.h>

// A transparent NSWindow is only half of it: SDL renders into a backing layer
// (CAMetalLayer for the accelerated path) that is created opaque, so an alpha
// of zero still composites onto black. Every layer in the view tree has to be
// told it is not opaque.
static void clear_layer_backgrounds(NSView *view)
{
    if (view == nil) {
        return;
    }
    CALayer *layer = [view layer];
    if (layer != nil) {
        [layer setOpaque:NO];
        [layer setBackgroundColor:[[NSColor clearColor] CGColor]];
    }
    for (NSView *child in [view subviews]) {
        clear_layer_backgrounds(child);
    }
}

extern "C" void eyes_mac_prepare_window(void *handle)
{
    NSWindow *window = (__bridge NSWindow *)handle;
    if (window == nil) {
        return;
    }
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
    // The device draws its own contact shadow; the system one would outline
    // the whole rectangular window.
    [window setHasShadow:NO];
    [window setLevel:NSFloatingWindowLevel];
    clear_layer_backgrounds([window contentView]);
}

extern "C" void eyes_mac_set_click_through(void *handle, int enabled)
{
    NSWindow *window = (__bridge NSWindow *)handle;
    if (window == nil) {
        return;
    }
    [window setIgnoresMouseEvents:(enabled != 0) ? YES : NO];
}
