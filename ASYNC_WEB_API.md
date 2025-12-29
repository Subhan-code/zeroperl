# Asynchronous Web API Support for zeroperl

## Overview

This document describes the implementation of asynchronous Web API support in zeroperl, an experimental build of Perl 5 running in WebAssembly (WASI) with reactor mode and Asyncify enabled.

## Architecture

### Async Execution Model

The implementation uses WASI reactor mode and Asyncify to enable pausing and resuming Perl execution during asynchronous operations. When an async operation is initiated:

1. The operation is registered in the async registry with a unique ID
2. A call is made to the JavaScript environment to start the async operation
3. The Perl execution is suspended using Asyncify's unwind mechanism
4. When the JavaScript operation completes, Perl execution is resumed using Asyncify's rewind mechanism

### Web API Bridge

The bridge exposes JavaScript Web APIs to Perl via imported WASM functions:

- `js_async_fetch`: Initiates an HTTP request in the JavaScript environment
- `js_async_timer`: Creates a timer in the JavaScript environment  
- `js_async_resolve_pending`: Checks for resolved operations in the JavaScript environment

### Perl-Side API

The Perl interface provides clean, intuitive functions:

- `fetch($url, %options)`: Initiates an async HTTP request
- `sleep_ms($milliseconds)`: Initiates an async timer
- `await($async_op)`: Suspends execution until the async operation completes

## Implementation Details

### Async Registry

The async registry (`async_web_api.c`) manages in-flight operations using shared WASM memory:

```c
typedef struct {
    int32_t id;                 // Unique operation ID
    async_op_type_t type;       // Operation type (fetch, timer, etc.)
    async_state_t state;        // Current state (pending, resolved, rejected)
    void *data;                 // Operation result data
    size_t data_size;           // Size of result data
    char *error_message;        // Error message if operation failed
} async_operation_t;
```

### WASM Exported Functions

The following functions are exported from the WASM module:

- `async_web_api_init()`: Initializes the async registry
- `async_fetch(url, method, headers, body)`: Initiates an async fetch operation
- `async_timer(delay_ms)`: Initiates an async timer operation
- `async_check_status(op_id, out_result, out_size, out_error)`: Checks operation status
- `async_wait_for_completion(op_id)`: Waits for an operation to complete (suspends Perl execution)
- `async_cleanup(op_id)`: Cleans up an operation

### JavaScript Host Interface

The JavaScript host must implement these functions and import them into the WASM module:

- `js_async_fetch(url, method, headers, body)`: Starts an HTTP fetch in JS environment
- `js_async_timer(delay_ms)`: Starts a timer in JS environment
- `js_async_resolve_pending()`: Checks for completed operations

## Usage Examples

### Basic HTTP Fetch

```perl
use AsyncWebAPI qw(fetch await);

my $fetch_op = fetch("https://httpbin.org/get");
my $result = await($fetch_op);
print "Fetch result: $result\n";
```

### Async Sleep

```perl
use AsyncWebAPI qw(sleep_ms await);

print "Before sleep\n";
my $timer_op = sleep_ms(1000);
await($timer_op);
print "After sleep\n";
```

### Concurrent Operations

```perl
use AsyncWebAPI qw(fetch sleep_ms await);

# Start multiple operations
my @operations;
push @operations, fetch("https://httpbin.org/get");
push @operations, sleep_ms(500);

# Wait for all operations
for my $op (@operations) {
    await($op);
}
```

## Build System Integration

The build system has been updated to include the new async functionality:

1. `async_web_api.c` is compiled to `async_web_api.o`
2. The object file is linked into the final WASM module
3. Asyncify is configured to handle the new import functions

## Error Handling

Errors in async operations are converted to Perl exceptions:

- JavaScript errors are captured and stored in the operation registry
- When `await()` is called on a failed operation, a Perl exception is thrown
- Error messages are preserved and accessible through the Perl exception mechanism

## Compatibility

The implementation works in:

- Browser environments
- Node.js
- WASI runtimes

## Implementation Files

- `stubs/async_web_api.h`: Header file with async API declarations
- `stubs/async_web_api.c`: Implementation of async registry and operations
- `stubs/zeroperl.c`: Integration with main zeroperl codebase
- `pipeline/build-wasm.sh`: Build system updates
- `AsyncWebAPI.pm`: Perl module interface
- `AsyncWebAPI.xs`: XS bindings for Perl module
- `demo_async.pl`: Example usage
- `example_host.js`: JavaScript host implementation example

## How Asyncify Integration Works

The asyncify mechanism is used to suspend Perl execution during async operations:

1. When `async_wait_for_completion()` is called and the operation is still pending:
   - `asyncify_start_unwind()` is called to save the current execution state
   - Control returns to the JavaScript environment
2. When JavaScript completes an operation:
   - The operation state is updated in the registry
   - The WASM module is called to resume execution
3. When execution resumes:
   - `asyncify_start_rewind()` is called to restore the execution state
   - The Perl code continues from where it left off

This allows Perl code to use synchronous-looking syntax (`await`) while actually being non-blocking at the JavaScript level.