package AsyncWebAPI;

use strict;
use warnings;
use Exporter qw(import);

our @EXPORT_OK = qw(fetch sleep_ms await);
our @EXPORT = @EXPORT_OK;

# XS function declarations
require XSLoader;
XSLoader::load('AsyncWebAPI');

# Perl wrapper functions for async operations

sub fetch {
    my ($url, %options) = @_;
    
    my $method = uc($options{method} // 'GET');
    my $headers = $options{headers} // {};
    my $body = $options{body} // '';
    
    # Convert headers to JSON string
    my $headers_json = _encode_headers($headers);
    
    # Call the C function to initiate the async fetch
    my $op_id = _async_fetch($url, $method, $headers_json, $body);
    
    if ($op_id < 0) {
        die "Failed to initiate fetch operation for URL: $url";
    }
    
    # Return an async operation handle that can be awaited
    return {
        op_id => $op_id,
        type => 'fetch',
        url => $url
    };
}

sub sleep_ms {
    my ($delay_ms) = @_;
    
    # Call the C function to initiate the async timer
    my $op_id = _async_timer(int($delay_ms));
    
    if ($op_id < 0) {
        die "Failed to initiate timer operation for delay: $delay_ms ms";
    }
    
    # Return an async operation handle that can be awaited
    return {
        op_id => $op_id,
        type => 'timer',
        delay => $delay_ms
    };
}

sub await {
    my ($async_op) = @_;
    
    if (ref($async_op) ne 'HASH' || !exists($async_op->{op_id})) {
        die "Invalid async operation handle passed to await";
    }
    
    # Wait for the operation to complete using the C function
    my $success = _async_wait_for_completion($async_op->{op_id});
    
    if (!$success) {
        # Check for error details
        my $status = _async_check_status($async_op->{op_id});
        my $error = _get_error_message($async_op->{op_id});
        
        # Clean up the operation
        _async_cleanup($async_op->{op_id});
        
        die "Async operation failed: $error" if $error;
        die "Async operation failed with status: $status";
    }
    
    # Get the result
    my $result = _get_operation_result($async_op->{op_id});
    
    # Clean up the operation
    _async_cleanup($async_op->{op_id});
    
    return $result;
}

# Helper functions

sub _encode_headers {
    my ($headers) = @_;
    
    return '{}' unless ref($headers) eq 'HASH';
    
    my @pairs;
    for my $key (keys %$headers) {
        push @pairs, "\"$key\":\"$headers->{$key}\"";
    }
    
    return '{' . join(',', @pairs) . '}';
}

sub _decode_json_response {
    my ($json_str) = @_;
    
    # Simple JSON parsing for basic response
    # In a real implementation, this would use a proper JSON parser
    # For now, just return the raw string
    return $json_str;
}

1;

__END__

=head1 NAME

AsyncWebAPI - Asynchronous Web API functions for zeroperl

=head1 SYNOPSIS

    use AsyncWebAPI;

    # Perform an async fetch
    my $fetch_op = fetch("https://httpbin.org/get");
    my $result = await($fetch_op);
    
    # Perform an async sleep
    my $sleep_op = sleep_ms(1000);
    await($sleep_op);

=head1 DESCRIPTION

This module provides asynchronous Web API functions for use in zeroperl.
The functions return immediately and allow Perl execution to continue,
while the actual operations are handled by the JavaScript environment.

=head1 FUNCTIONS

=head2 fetch($url, %options)

Initiates an asynchronous HTTP request.

Options:
- method: HTTP method (GET, POST, etc.), defaults to GET
- headers: Hash reference of HTTP headers
- body: Request body content

Returns an async operation handle that can be awaited.

=head2 sleep_ms($milliseconds)

Initiates an asynchronous timer for the specified number of milliseconds.

Returns an async operation handle that can be awaited.

=head2 await($async_op)

Waits for an async operation to complete. This function will suspend
the Perl execution until the operation is complete, without blocking
the JavaScript environment.

=head1 AUTHOR

zeroperl project

=head1 LICENSE

This software is Copyright (c) 2025 by the zeroperl project.

This is free software, licensed under:

  The Artistic License 2.0

=cut