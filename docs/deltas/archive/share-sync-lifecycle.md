# Share Synchronized Access And Teardown

Status: imported
Target: docs/plans/stdlib/soon-thread.md#sharing-wrappers
Owner: -
Related: -

### docs/plans/stdlib/soon-thread.md#sharing-wrappers

- add: `### Proposed Synchronized Access`
- add: This is a proposed runtime contract, not an implemented API or a concrete Zith-- signature. If a `Share<T>` is registered with a `Disposer`, the `Disposer` remains responsible for destroying `T`. Copying a `Share<T>` copies an access handle, not ownership of `T` or an extension of its lifetime. The control block remains valid while handles exist, including after `T` has been destroyed.
- add: `sync(share, action)` waits while another action holds the share and then calls `action` with a `lend T`. `trySync(share, action)` does not wait and reports `Busy` when access is contended. Both report `Closed` if teardown has begun before the action can start. The lock stays internal and the action receives neither a mutex nor a guard.
- add: Teardown closes admission to new actions. Calls waiting for access when teardown begins fail with `Closed`; actions already running are allowed to finish. The `Disposer` destroys `T` only after those active actions have finished. A surviving handle can still inspect the control block and receives `Closed` instead of accessing the destroyed value.
- add: Access failures are distinct from errors returned by the action. A conceptual result is `Result<R, SyncError<E>>`, where `SyncError<E>` distinguishes `Closed` from `Action(E)`, and `trySync` also distinguishes `Busy`. The exact type and signature remain subject to the Zith-- type system.
- add: The lock is released on every action exit, including an action error. An action must not re-enter `sync` or `trySync` on the same `Share`, and must not return or retain its `lend T`. Until the compiler can enforce non-escape, that restriction is a manual precondition; a future `noescape`-style annotation may express it.

### docs/plans/stdlib/soon-thread.md#open-decisions

- change: The concrete `Share<T>` and `Revokable<T>` method contracts and how the owner revokes and waits for acknowledgement. -> The remaining `Share<T>` method surface beyond the proposed synchronized-access contract, and the `Revokable<T>` access/revocation contract and acknowledgement behavior.

## Rationale

The shared value must not outlive its `Disposer`-owned cleanup obligation, while concurrent actions need a defined close-and-drain boundary before destruction. Keeping the API and non-escape enforcement explicitly proposed avoids implying compiler or runtime guarantees that Zith-- does not yet provide.
