#!/usr/bin/perl
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Rewrite a CSV stream with uniform quoting and escaping so that two exports
# can be diffed without formatting noise.
#
# Usage:
#   scripts/normalize-csv.sh < in.csv > out.csv

use strict;
use warnings;
use Text::CSV;

# Create a new CSV object
my $csv = Text::CSV->new({
    binary => 1,       # Allow UTF-8
    auto_diag => 1,    # Report errors
    quote_char => '"', # Double quote is the quote character
    escape_char => '"',# Double quote is also the escape character
});

# Read the header row
if (my $line = <STDIN>) {
    if ($csv->parse($line)) {
        my @header = $csv->fields();
        $csv->say(\*STDOUT, \@header);
    } else {
        warn "Line could not be parsed: $line\n";
    }
} else {
    die "No header row found\n";
}

# Read each row
while (my $line = <STDIN>) {
    chomp $line;
    if ($csv->parse($line)) {
        my @fields = $csv->fields();
        $csv->say(\*STDOUT, \@fields);
    } else {
        warn "Line could not be parsed: $line\n";
    }
}