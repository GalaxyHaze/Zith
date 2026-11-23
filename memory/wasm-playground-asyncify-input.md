# WASM Playground Asyncify Input

The browser's `zith.host_read_byte` import must call
`runtimeModule.Asyncify.handleSleep` for every read. Returning a queued byte or
EOF directly can leave Firefox showing the submitted line while the guest
remains suspended. Inside `handleSleep`, wake immediate bytes and EOF through
`queueMicrotask`, and set `pendingGuestRead` only when waiting for user input.
Keep the Firefox acceptance check because the Node glue harness alone did not
catch this browser integration failure.
