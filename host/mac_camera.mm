// macOS webcam capture for the simulator's fake CameraService. AVFoundation
// delivers BGRA frames on its own queue; the newest one is kept under a lock
// and handed out by copy. The Mac has one camera, so both of the product's
// lenses map to it — the point is exercising the real CameraService interface
// (and the apps written against it) years before the dual-camera hardware
// exists.
//
// Camera access needs an NSCameraUsageDescription: the Makefile embeds
// host/sim_info.plist into the binary (__TEXT,__info_plist) so the bare CLI
// tool can pass TCC; the permission prompt is attributed to the terminal.

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Vision/Vision.h>

#include <cstring>
#include <mutex>
#include <vector>

namespace {

std::mutex g_frame_mutex;
std::vector<unsigned char> g_frame_bgra;  // tightly packed rows, width * 4
int g_frame_width = 0;
int g_frame_height = 0;

AVCaptureSession *g_session = nil;
id g_delegate = nil;
dispatch_queue_t g_queue = nullptr;

// Blink detection (Vision face landmarks, throttled to every 4th frame).
// A blink is open -> closed -> open; the counter accumulates until taken.
std::mutex g_blink_mutex;
int g_blink_count = 0;
bool g_eyes_were_closed = false;
int g_vision_frame_divider = 0;

// Eye openness: bounding-box height over width of the landmark points.
float eye_openness(VNFaceLandmarkRegion2D *eye)
{
    if (eye == nil || eye.pointCount < 3) {
        return -1.0F;
    }
    const CGPoint *points = eye.normalizedPoints;
    CGFloat min_x = points[0].x, max_x = points[0].x;
    CGFloat min_y = points[0].y, max_y = points[0].y;
    for (NSUInteger i = 1; i < eye.pointCount; ++i) {
        min_x = MIN(min_x, points[i].x);
        max_x = MAX(max_x, points[i].x);
        min_y = MIN(min_y, points[i].y);
        max_y = MAX(max_y, points[i].y);
    }
    const CGFloat width = max_x - min_x;
    if (width <= 0.0) {
        return -1.0F;
    }
    return static_cast<float>((max_y - min_y) / width);
}

void detect_blink(CVImageBufferRef image)
{
    VNDetectFaceLandmarksRequest *request = [[VNDetectFaceLandmarksRequest alloc] init];
    VNImageRequestHandler *handler =
        [[VNImageRequestHandler alloc] initWithCVPixelBuffer:image options:@{}];
    NSError *error = nil;
    if (![handler performRequests:@[ request ] error:&error]) {
        return;
    }
    VNFaceObservation *face = request.results.firstObject;
    if (face == nil || face.landmarks == nil) {
        return;
    }
    const float left = eye_openness(face.landmarks.leftEye);
    const float right = eye_openness(face.landmarks.rightEye);
    if (left < 0.0F || right < 0.0F) {
        return;
    }
    const float openness = (left + right) * 0.5F;
    std::lock_guard<std::mutex> lock(g_blink_mutex);
    if (!g_eyes_were_closed && openness < 0.16F) {
        g_eyes_were_closed = true;
    } else if (g_eyes_were_closed && openness > 0.24F) {
        g_eyes_were_closed = false;
        ++g_blink_count;
    }
}

}  // namespace

@interface EyesCameraDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@end

@implementation EyesCameraDelegate
- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection *)connection
{
    (void)output;
    (void)connection;
    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (image == nullptr) {
        return;
    }
    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const int width = static_cast<int>(CVPixelBufferGetWidth(image));
    const int height = static_cast<int>(CVPixelBufferGetHeight(image));
    const std::size_t stride = CVPixelBufferGetBytesPerRow(image);
    const unsigned char *base =
        static_cast<const unsigned char *>(CVPixelBufferGetBaseAddress(image));
    if (base != nullptr && width > 0 && height > 0) {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U;
        g_frame_bgra.resize(row_bytes * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y) {
            std::memcpy(g_frame_bgra.data() + static_cast<std::size_t>(y) * row_bytes,
                        base + static_cast<std::size_t>(y) * stride, row_bytes);
        }
        g_frame_width = width;
        g_frame_height = height;
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    if (++g_vision_frame_divider >= 4) {
        g_vision_frame_divider = 0;
        detect_blink(image);
    }
}
@end

// Blinks detected since the last call (and clears the counter).
extern "C" int eyes_mac_camera_take_blinks(void)
{
    std::lock_guard<std::mutex> lock(g_blink_mutex);
    const int count = g_blink_count;
    g_blink_count = 0;
    return count;
}

// 1 on success (session running; frames may still take a beat), 0 when there
// is no camera or the user denied access.
extern "C" int eyes_mac_camera_start(void)
{
    if (g_session != nil) {
        return 1;
    }
    const AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted) {
        return 0;
    }
    if (status == AVAuthorizationStatusNotDetermined) {
        dispatch_semaphore_t gate = dispatch_semaphore_create(0);
        __block BOOL granted = NO;
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL ok) {
                                     granted = ok;
                                     dispatch_semaphore_signal(gate);
                                 }];
        dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER);
        if (!granted) {
            return 0;
        }
    }
    AVCaptureDevice *device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    if (device == nil) {
        return 0;
    }
    NSError *error = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device
                                                                        error:&error];
    if (input == nil) {
        return 0;
    }
    AVCaptureSession *session = [[AVCaptureSession alloc] init];
    if ([session canSetSessionPreset:AVCaptureSessionPreset640x480]) {
        session.sessionPreset = AVCaptureSessionPreset640x480;
    }
    if (![session canAddInput:input]) {
        return 0;
    }
    [session addInput:input];
    AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
    output.videoSettings =
        @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
    output.alwaysDiscardsLateVideoFrames = YES;
    g_delegate = [[EyesCameraDelegate alloc] init];
    g_queue = dispatch_queue_create("eyes.sim.camera", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:(EyesCameraDelegate *)g_delegate queue:g_queue];
    if (![session canAddOutput:output]) {
        return 0;
    }
    [session addOutput:output];
    [session startRunning];
    g_session = session;
    return 1;
}

extern "C" void eyes_mac_camera_stop(void)
{
    if (g_session != nil) {
        [g_session stopRunning];
        g_session = nil;
    }
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    g_frame_width = 0;
    g_frame_height = 0;
}

// Copies the newest BGRA frame into dst (tightly packed). Returns 1 and sets
// *width / *height when a frame fit; 0 when none has arrived yet or capacity
// is too small.
extern "C" int eyes_mac_camera_latest(unsigned char *dst, int dst_capacity, int *width,
                                      int *height)
{
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    if (g_frame_width == 0 || g_frame_height == 0) {
        return 0;
    }
    const std::size_t bytes = static_cast<std::size_t>(g_frame_width) *
                              static_cast<std::size_t>(g_frame_height) * 4U;
    if (dst == nullptr || static_cast<std::size_t>(dst_capacity) < bytes) {
        return 0;
    }
    std::memcpy(dst, g_frame_bgra.data(), bytes);
    *width = g_frame_width;
    *height = g_frame_height;
    return 1;
}
