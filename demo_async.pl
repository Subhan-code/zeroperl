#!/usr/bin/perl

# Demo script showing asynchronous Web API functionality in zeroperl
use strict;
use warnings;

# Import the AsyncWebAPI module
use AsyncWebAPI qw(fetch sleep_ms await);

print "Starting async operations demo...\n";

# Example 1: Non-blocking HTTP fetch
print "\n1. Performing async HTTP fetch...\n";
my $fetch_op = fetch("https://httpbin.org/get", method => 'GET');
print "Fetch operation initiated, continuing execution...\n";

# Example 2: Async timer (non-blocking sleep)
print "2. Starting async timer for 1000ms...\n";
my $timer_op = sleep_ms(1000);
print "Timer started, continuing execution...\n";

# Example 3: Concurrent operations
print "3. Starting multiple concurrent operations...\n";
my @operations;

# Multiple fetches
push @operations, fetch("https://httpbin.org/delay/1", method => 'GET');
push @operations, fetch("https://httpbin.org/headers", method => 'GET');

# Another timer
push @operations, sleep_ms(500);

print "All operations started, now awaiting results...\n";

# Wait for the timer first
print "Waiting for timer...\n";
await($timer_op);
print "Timer completed!\n";

# Wait for the first fetch
print "Waiting for first fetch...\n";
my $fetch_result = await($fetch_op);
print "Fetch completed!\n";

# Wait for concurrent operations
for my $i (0 .. $#operations) {
    print "Waiting for operation " . ($i + 1) . "...\n";
    my $result = await($operations[$i]);
    print "Operation " . ($i + 1) . " completed!\n";
}

print "\nAll async operations completed successfully!\n";

# Example 4: Error handling
print "\n4. Testing error handling...\n";
eval {
    my $error_op = fetch("https://invalid-url-that-does-not-exist.com");
    await($error_op);
};
if ($@) {
    print "Caught error as expected: $@\n";
} else {
    print "No error occurred\n";
}

print "\nDemo completed!\n";