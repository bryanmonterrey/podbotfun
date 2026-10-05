// Speech input for the simulator's voice loop, two modes over one audio
// engine (only one tap on the mic is allowed):
//   PTT  — push-to-talk: hold a key, speak, release, get the final transcript.
//   WAKE — continuous, opt-in: always listening for the wake word ("hey
//          <name>"); when heard, the words that follow become the command.
//          Half-duplex by design (mic closes while the device speaks). macOS
//          ends a recognition task after a silence; we restart it to keep
//          listening, and hard-restart past its ~1 min per-task ceiling.
//
// Needs NSMicrophoneUsageDescription + NSSpeechRecognitionUsageDescription
// (host/sim_info.plist). Prompts attribute to the terminal.

#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

enum class Mode { idle, ptt, wake };

std::mutex g_mutex;
std::string g_transcript;   // PTT final result, awaiting pickup
std::string g_command;      // WAKE command (text after the wake word)
std::string g_wake_word = "hey podbot";
bool g_awake = false;       // WAKE: heard the word, now capturing the command
Mode g_mode = Mode::idle;

AVAudioEngine *g_engine = nil;
SFSpeechRecognizer *g_recognizer = nil;
SFSpeechAudioBufferRecognitionRequest *g_request = nil;
SFSpeechRecognitionTask *g_task = nil;

std::string lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool authorize()
{
    if ([SFSpeechRecognizer authorizationStatus] ==
        SFSpeechRecognizerAuthorizationStatusNotDetermined) {
        dispatch_semaphore_t gate = dispatch_semaphore_create(0);
        [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus s) {
            (void)s;
            dispatch_semaphore_signal(gate);
        }];
        dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER);
    }
    if ([SFSpeechRecognizer authorizationStatus] != SFSpeechRecognizerAuthorizationStatusAuthorized)
        return false;
    if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] ==
        AVAuthorizationStatusNotDetermined) {
        dispatch_semaphore_t gate = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                 completionHandler:^(BOOL g) {
                                     (void)g;
                                     dispatch_semaphore_signal(gate);
                                 }];
        dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER);
    }
    return [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] ==
           AVAuthorizationStatusAuthorized;
}

// Handle a recognition result string for the active mode.
void on_result(const std::string &text, bool is_final)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_mode == Mode::ptt) {
        if (is_final) g_transcript = text;
        return;
    }
    if (g_mode == Mode::wake) {
        const std::string low = lower(text);
        const std::string word = lower(g_wake_word);
        const std::size_t at = low.rfind(word);
        if (at != std::string::npos) {
            g_awake = true;
            // The command is whatever follows the wake word.
            std::string cmd = text.substr(std::min(text.size(), at + word.size()));
            while (!cmd.empty() && (cmd.front() == ' ' || cmd.front() == ',')) cmd.erase(cmd.begin());
            if (is_final && !cmd.empty()) {
                g_command = cmd;
                g_awake = false;
            }
        }
    }
}

bool start_recognition(bool partial)
{
    g_request = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
    g_request.shouldReportPartialResults = partial ? YES : NO;
    AVAudioInputNode *input = g_engine.inputNode;
    AVAudioFormat *fmt = [input outputFormatForBus:0];
    [input removeTapOnBus:0];
    SFSpeechAudioBufferRecognitionRequest *req = g_request;
    [input installTapOnBus:0
                bufferSize:1024
                    format:fmt
                     block:^(AVAudioPCMBuffer *buf, AVAudioTime *when) {
                         (void)when;
                         [req appendAudioPCMBuffer:buf];
                     }];
    [g_engine prepare];
    NSError *err = nil;
    if (![g_engine startAndReturnError:&err]) {
        [input removeTapOnBus:0];
        return false;
    }
    g_task = [g_recognizer
        recognitionTaskWithRequest:g_request
                     resultHandler:^(SFSpeechRecognitionResult *result, NSError *taskErr) {
                         if (result != nil) {
                             const char *s = [result.bestTranscription.formattedString UTF8String];
                             on_result(s ? s : "", result.isFinal);
                         }
                         // In WAKE mode keep the ears open: when a task ends
                         // (silence or error), start a fresh one.
                         if ((result != nil && result.isFinal) || taskErr != nil) {
                             if (g_mode == Mode::wake) {
                                 [g_engine.inputNode removeTapOnBus:0];
                                 [g_engine stop];
                                 dispatch_after(
                                     dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.15 * NSEC_PER_SEC)),
                                     dispatch_get_main_queue(), ^{
                                         if (g_mode == Mode::wake) start_recognition(true);
                                     });
                             }
                         }
                     }];
    return true;
}

void teardown()
{
    if (g_engine != nil) {
        [g_engine.inputNode removeTapOnBus:0];
        [g_engine stop];
    }
    if (g_request != nil) [g_request endAudio];
    g_task = nil;
    g_request = nil;
}

}  // namespace

// ---- push-to-talk ------------------------------------------------------
extern "C" int eyes_mac_speech_begin(void)
{
    if (g_mode == Mode::ptt) return 1;
    if (g_mode == Mode::wake) return 0;  // exclusive; disable wake first
    if (!authorize()) return 0;
    if (g_recognizer == nil) g_recognizer = [[SFSpeechRecognizer alloc] init];
    if (g_recognizer == nil || !g_recognizer.isAvailable) return 0;
    if (g_engine == nil) g_engine = [[AVAudioEngine alloc] init];
    g_mode = Mode::ptt;
    if (!start_recognition(false)) {
        g_mode = Mode::idle;
        return 0;
    }
    return 1;
}

extern "C" void eyes_mac_speech_end(void)
{
    if (g_mode != Mode::ptt) return;
    teardown();
    g_mode = Mode::idle;
}

extern "C" int eyes_mac_speech_take_transcript(char *out, int capacity)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_transcript.empty()) return 0;
    std::snprintf(out, static_cast<std::size_t>(capacity), "%s", g_transcript.c_str());
    g_transcript.clear();
    return 1;
}

// ---- wake word ---------------------------------------------------------
extern "C" int eyes_mac_wake_enable(const char *wake_word)
{
    if (g_mode == Mode::wake) return 1;
    if (g_mode == Mode::ptt) return 0;
    if (!authorize()) return 0;
    if (g_recognizer == nil) g_recognizer = [[SFSpeechRecognizer alloc] init];
    if (g_recognizer == nil || !g_recognizer.isAvailable) return 0;
    if (g_engine == nil) g_engine = [[AVAudioEngine alloc] init];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (wake_word != nullptr && wake_word[0] != '\0') g_wake_word = wake_word;
        g_awake = false;
        g_command.clear();
    }
    g_mode = Mode::wake;
    if (!start_recognition(true)) {
        g_mode = Mode::idle;
        return 0;
    }
    return 1;
}

extern "C" void eyes_mac_wake_disable(void)
{
    if (g_mode != Mode::wake) return;
    g_mode = Mode::idle;
    teardown();
}

// 1 = a command was captured after the wake word (copied out and cleared).
extern "C" int eyes_mac_wake_take_command(char *out, int capacity)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_command.empty()) return 0;
    std::snprintf(out, static_cast<std::size_t>(capacity), "%s", g_command.c_str());
    g_command.clear();
    return 1;
}

// 1 while the wake word has been heard and the device is capturing a command.
extern "C" int eyes_mac_wake_is_awake(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_awake ? 1 : 0;
}
