#!/usr/bin/perl

# Simple test to verify async functionality
use strict;
use warnings;

# Try to load the AsyncWebAPI module
eval {
    require AsyncWebAPI;
    AsyncWebAPI->import(qw(fetch sleep_ms await));
};

if ($@) {
    print "Error loading AsyncWebAPI module: $@\n";
    exit 1;
}

print "AsyncWebAPI module loaded successfully!\n";

# Test basic functionality
print "Testing async timer...\n";
my $timer = sleep_ms(100);
print "Timer started, now awaiting...\n";
await($timer);
print "Timer completed!\n";

print "Async functionality test completed successfully!\n";