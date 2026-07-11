#pragma once

namespace mm {

/// The "one active instance" election as a core RAII member. Several instances of a module `T` may
/// exist (two mics, say), but exactly one is "the active one" a consumer reaches through the static
/// `active()` accessor. A module declares `ActiveInstance<T> seat_{*this};` and calls `claim()` when
/// it becomes live and `vacate()` when it releases — the four moves that used to be hand-copied
/// (claim, vacate-if-mine, vacate-on-destruct, survivor-reclaims-the-empty-seat) all live here now.
///
/// **The seat is per-`T`.** `active()` returns the one live winner, or nullptr when the seat is empty
/// — the consumer null-checks (an audio effect falls back to a silent frame, a driver skips).
///
/// **`claim()` is idempotent claim-if-empty**: the first live instance wins and a later claimant does
/// NOT displace it. That single semantic serves both roles — a module calls `claim()` in its build
/// hook to take the seat, AND (if it wants a survivor to take over an emptied seat) calls the same
/// `claim()` in its tick, where it does nothing while the seat is held and reclaims it the moment the
/// holder vacated. So there is no separate "reclaim" method; the reclaim IS an idempotent re-claim.
///
/// **RAII vacate is the dangling-static guard.** The destructor vacates if this instance holds the
/// seat, so a destroyed module can never leave `active()` pointing at freed memory — the bug this
/// primitive exists to make unrepresentable. Copy and move are deleted: the seat is tied to a
/// specific instance by reference, so relocating it would dangle `self_`; like the sibling
/// `ScratchBuffer`, it is a fixed, non-relocatable owned member.
///
/// A module member destructs BEFORE the module's base subobject, so the destructor's `vacate()` reads
/// only this object's own `seat_` (a static) and `self_` (a reference) — never a call into the
/// half-destroyed owner. Safe in any construct/claim/vacate/destruct order.
template <class T>
class ActiveInstance {
public:
    explicit ActiveInstance(T& self) : self_(self) {}
    ~ActiveInstance() { vacate(); }

    ActiveInstance(const ActiveInstance&) = delete;
    ActiveInstance& operator=(const ActiveInstance&) = delete;
    ActiveInstance(ActiveInstance&&) = delete;
    ActiveInstance& operator=(ActiveInstance&&) = delete;

    /// Take the seat if it is empty (first live wins). Idempotent, so calling it again — e.g. from a
    /// tick — is a no-op while held and the survivor's reclaim once the holder has vacated.
    void claim()  { if (!seat_) seat_ = &self_; }
    /// Give up the seat, but only if this instance holds it (never yanks another's seat).
    void vacate() { if (seat_ == &self_) seat_ = nullptr; }
    /// Does this instance currently hold the seat?
    bool seated() const { return seat_ == &self_; }

    /// The one live winner, or nullptr when the seat is empty. Consumers null-check.
    static T* active() { return seat_; }

private:
    T& self_;
    static inline T* seat_ = nullptr;   // one seat per T
};

} // namespace mm
