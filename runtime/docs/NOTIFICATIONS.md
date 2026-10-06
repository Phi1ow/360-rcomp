# Notification listeners

The actual GTA IV host entry reached `XamNotifyCreateListener` at caller
`0x827DE0EC`, with mask1 and maximum version3. Its implementation allocates a
real typed listener handle. A listener owns a FIFO queue, filters notification
category (ID bits25..30) and version (bits16..24), and is manually waitable while
the queue is nonempty. A matching-ID dequeue removes the earliest matching
entry while preserving the others. Duplicate handles share the same queue;
closing one does not invalidate the other or an already-retained waiter.

ABI source: public rexglue-sdk commit
`c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`, `src/kernel/xam/xam_notify.cpp`,
`src/system/xnotifylistener.cpp`, and `include/rex/system/xnotifylistener.h`,
derived from public Xenia (BSD-3). No reference kernel implementation is copied.
The mask uses the full64-bit GPR; maximum version is the next GPR, as in the
actual caller. `XNotifyGetNext` returns a Boolean and zeroes writable outputs
when empty or the handle is invalid. The parameter destination is optional.
Invalid output memory is rejected before removing a queued message.

Creation injects no fictional sign-in, UI-opening or connection event.
`publish_xam_notification` is the producer boundary for actual state transitions.
It filters subscribers and wakes the real runtime dispatcher. Tests exercise
production producer/consumer, selective dequeue, high masks, empty queues,
pointer failures, waits, duplicates and runtime cleanup. A queue is bounded to
65,536 entries. Publish reports OutOfMemory on resource failure; recipients
already delivered to may retain the message, so callers must not blindly retry
a failed broadcast. Existing messages are never silently dropped. Successful
publication means matching deliveries, possibly zero with no subscriber.

UI placement/rendering, versions above10 and other notification imports remain
unsupported. `XNotifyPositionUI` (0x28C) records the requested popup position (no UI is drawn).

`XamShowSigninUI` (0x2BC): with no sign-in service the system UI opens and is
dismissed without a change. It publishes `XN_SYS_UI` (0x9) parameter 1 then 0
and returns 0; no sign-in-changed notification is generated and every user slot
stays signed out (see `XAM.md`). Found when A ("Yes") was chosen on GTA IV's
"You are not signed in" prompt.
