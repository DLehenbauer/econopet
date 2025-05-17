#!/usr/bin/perl

# Convert PET concordance exported by 'cbmsymbols' tool to CSV format
# (See https://github.com/ethandicks/cbmsymbols)

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

while (<>) {
    # Skip lines that don't contain data (e.g., header lines, empty lines)
    next unless /^\w/;

    # Extract columns based on fixed offsets
    my $label       = substr($_,  0,  9);
    my $version_1_0 = substr($_, 10,  8);
    my $version_2_0 = substr($_, 18,  8);
    my $version_4_0 = substr($_, 26,  8);
    my $description = substr($_, 34);

    # Clean up whitespace
    $label       = trim($label);
    $version_1_0 = trim($version_1_0);
    $version_2_0 = trim($version_2_0);
    $version_4_0 = trim($version_4_0);
    $description = trim($description);

    # Remove leading dollar signs
    $version_1_0 =~ s/^\$//;
    $version_2_0 =~ s/^\$//;
    $version_4_0 =~ s/^\$//;

    # Create an array of the data
    my @row = ($version_1_0, $version_2_0, $version_4_0, $label, $description);

    # Output as CSV
    $csv->say(\*STDOUT, \@row);
}

sub trim {
    my $str = shift;
    $str =~ s/^\s+|\s+$//g;
    return $str;
}
