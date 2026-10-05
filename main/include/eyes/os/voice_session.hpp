#pragma once

// The voice stream as the face sees it. One live bidirectional session per
// device (docs/VOICE_AGENT.md): a wake word or the PWR key opens it, the
// model decides when the user's turn ends, the reply streams back while it is
// generated, and talking over the reply cuts it (barge-in). This is the state
// machine that maps those edges to the face; the transport (I2S, WebSocket,
// the Mac recognizer) is somebody else's problem and reports edges here.
//
// Platform-free, allocation-free, no wall clock: time is a parameter, like
// the engine. The device holds `listening` / `thinking` on the engine from
// state(); the simulator does the same, so both feel identical.

#include <cstdint>

#include "eyes/eye_engine.hpp"

namespace eyes {

enum class VoiceState : std::uint8_t { idle, listening, thinking, speaking };

class VoiceSession {
  public:
    // A session with no transport fitted still answers the wake edge: the
    // face perks up so the trigger is felt, then it lets go after this long
    // instead of listening to nothing. With a transport, the idle timeout is
    // the contract's 30 s of silence.
    static constexpr std::uint32_t kNoTransportHoldMs = 2500U;
    static constexpr std::uint32_t kIdleTimeoutMs = 30000U;

    void set_transport_available(bool available) { transport_ = available; }
    bool transport_available() const { return transport_; }

    // Wake word heard or PWR pressed. idle -> listening; speaking ->
    // listening is the barge-in (the caller clears its playback ring);
    // listening just refreshes the idle deadline.
    void wake(std::uint32_t now_ms)
    {
        if (state_ == VoiceState::speaking) {
            barge_ins_ = static_cast<std::uint8_t>(barge_ins_ + 1U);
        }
        if (state_ == VoiceState::thinking) {
            return;  // the model is mid-turn; the next edge decides
        }
        enter(VoiceState::listening, now_ms);
    }

    // The STT model decided the user is done talking.
    void end_of_turn(std::uint32_t now_ms)
    {
        if (state_ == VoiceState::listening) {
            enter(VoiceState::thinking, now_ms);
        }
    }

    // First reply audio arrived.
    void reply_started(std::uint32_t now_ms)
    {
        if (state_ != VoiceState::idle) {
            enter(VoiceState::speaking, now_ms);
        }
    }

    // Reply audio drained. The session stays open across turns: back to
    // listening, and the idle timeout closes it if nobody speaks.
    void reply_finished(std::uint32_t now_ms)
    {
        if (state_ == VoiceState::speaking) {
            enter(VoiceState::listening, now_ms);
        }
    }

    // "That's all", the socket closed, or an error: back to the quiet face.
    void close(std::uint32_t now_ms) { enter(VoiceState::idle, now_ms); }

    // Deadlines. Only listening times out; thinking and speaking end on
    // edges the transport reports.
    void tick(std::uint32_t now_ms)
    {
        if (state_ == VoiceState::listening &&
            static_cast<std::int32_t>(now_ms - deadline_ms_) >= 0) {
            enter(VoiceState::idle, now_ms);
        }
    }

    VoiceState state() const { return state_; }
    bool open() const { return state_ != VoiceState::idle; }
    // One-shot: true once per state change, so the caller applies the face
    // hold exactly on the edge (the engine's mood API is edge-driven).
    bool consume_changed()
    {
        const bool changed = changed_;
        changed_ = false;
        return changed;
    }
    // Barge-ins since the last consume, for the transport to clear playback.
    std::uint8_t consume_barge_ins()
    {
        const std::uint8_t count = barge_ins_;
        barge_ins_ = 0U;
        return count;
    }

    // The face for a state. speaking has no mood of its own: the audio bars
    // replace the eyes while the reply plays, so the engine is released.
    static bool expression_for(VoiceState state, Expression &out)
    {
        switch (state) {
            case VoiceState::listening:
                out = Expression::listening;
                return true;
            case VoiceState::thinking:
                out = Expression::thinking;
                return true;
            case VoiceState::speaking:
            case VoiceState::idle:
                return false;
        }
        return false;
    }

    // Applies the current state to the engine on a change: hold the state's
    // mood, or release it. Blinks once on the way back to idle, the shell's
    // transition beat.
    void apply_to(EyeEngine &engine)
    {
        if (!consume_changed()) {
            return;
        }
        Expression expression{};
        if (expression_for(state_, expression)) {
            engine.hold_expression(expression);
        } else {
            engine.release_expression();
            if (state_ == VoiceState::idle) {
                engine.request_blink();
            }
        }
    }

  private:
    void enter(VoiceState next, std::uint32_t now_ms)
    {
        if (next == VoiceState::listening) {
            deadline_ms_ = now_ms + (transport_ ? kIdleTimeoutMs : kNoTransportHoldMs);
        }
        if (next != state_) {
            state_ = next;
            changed_ = true;
        }
    }

    VoiceState state_{VoiceState::idle};
    std::uint32_t deadline_ms_{0U};
    bool transport_{false};
    bool changed_{false};
    std::uint8_t barge_ins_{0U};
};

}  // namespace eyes
