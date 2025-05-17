#!/usr/bin/perl

use strict;
use warnings;
use Text::CSV;
use Tie::IxHash;

# Function to load a CSV file into a hash keyed by label
sub load_csv {
    my ($filename) = @_;
    my %data;
    tie %data, 'Tie::IxHash';  # Preserve insertion order

    my $csv = Text::CSV->new({ binary => 1, auto_diag => 1, eol => "\n" });

    open(my $fh, "<", $filename) or die "Cannot open $filename: $!";

    # Read and skip the header row
    my $header_line = <$fh>;
    if ($header_line) {
        $csv->parse($header_line); # Parse header to advance the parser
    }

    my $line_number = 1;
    while (my $line = <$fh>) {
        $line_number++;
        chomp $line;
        $line =~ s/\r//g;  # Remove carriage returns
        if ($csv->parse($line)) {
            my @fields = $csv->fields();
            next unless @fields == 5;  # Skip incomplete rows

            my ($addr1, $addr2, $addr4, $label, $description) = @fields;

            # Normalize label and addresses (lowercase)
            $label = lc $label;
            $addr1 = lc $addr1;
            $addr2 = lc $addr2;
            $addr4 = lc $addr4;

            $data{$label} = {
                addr1 => $addr1,
                addr2 => $addr2,
                addr4 => $addr4,
                description => $description,
                line_number => $line_number,
            };
        } else {
            warn "Could not parse line: $line\n";
        }
    }
    close $fh;
    return %data;
}

# Function to check for labels present in one hash but not in another
sub check_missing_labels {
    my ($data1, $data2, $file1, $file2, $diff_count) = @_;

    # Collect missing labels and their addr4 values
    my @missing_labels;

    # Iterate through keys in the order they appear in data1
    my @labels1 = keys %{$data1};
    foreach my $label (@labels1) {
        if (!exists ${$data2}{$label}) {
            my $d1 = ${$data1}{$label};
            push @missing_labels, { label => $label, data => $d1 };
        }
    }

    # Sort missing labels by line number
    @missing_labels = sort { $a->{data}->{line_number} <=> $b->{data}->{line_number} } @missing_labels;

    # Report missing labels in sorted order
    foreach my $missing_label_ref (@missing_labels) {
        my $label = $missing_label_ref->{label};
        my $d1 = $missing_label_ref->{data};

        print "Label '$label' exists in $file1 but not in $file2\n";
        print "  $file1:$d1->{line_number} Label '$label' ($d1->{addr1}, $d1->{addr2}, $d1->{addr4}, $d1->{description})\n";

        # Find labels in data2 with matching addresses
        my @matching_labels;
        foreach my $label2 (keys %{$data2}) {
            my $d2 = ${$data2}{$label2};
            if (lc($d1->{addr1}) eq lc($d2->{addr1}) ||
                lc($d1->{addr2}) eq lc($d2->{addr2}) ||
                lc($d1->{addr4}) eq lc($d2->{addr4})) {
                push @matching_labels, $label2;
            }
        }

        if (@matching_labels) {
            print "  Labels in $file2 with matching addresses:\n";
            foreach my $match_label (@matching_labels) {
                my $d2 = ${$data2}{$match_label};
                print "    $file2:$d2->{line_number} Label '$match_label' ($d2->{addr1}, $d2->{addr2}, $d2->{addr4}, $d2->{description})\n";
            }
        }

        $diff_count++;
    }
    return $diff_count;
}

# Function to compare addresses, considering "-" and "----" as equal
sub are_addresses_equal {
    my ($addr1, $addr2) = @_;
    $addr1 = "" if ($addr1 eq "-" || $addr1 eq "----");
    $addr2 = "" if ($addr2 eq "-" || $addr2 eq "----");
    return ($addr1 eq $addr2);
}

# Load the two CSV files
my $file1 = "memorymap-ocr.csv";
my $file2 = "memorymap-gen.csv";

my %data1 = load_csv($file1);
my %data2 = load_csv($file2);

# Compare the data
print "Comparing $file1 and $file2...\n";

my $diff_count = 0;

# Check for labels present in data1 but not in data2
$diff_count = check_missing_labels(\%data1, \%data2, $file1, $file2, $diff_count);

# Check for labels present in data2 but not in data1
$diff_count = check_missing_labels(\%data2, \%data1, $file2, $file1, $diff_count);

# Check for differences in common labels
my @labels1 = keys %data1;
foreach my $label (@labels1) {
    next unless exists $data2{$label}; # Skip labels only in one file

    my $d1 = $data1{$label};
    my $d2 = $data2{$label};

    if (!are_addresses_equal($d1->{addr1}, $d2->{addr1}) ||
        !are_addresses_equal($d1->{addr2}, $d2->{addr2}) ||
        !are_addresses_equal($d1->{addr4}, $d2->{addr4}) ||
        ($d1->{description} ne $d2->{description})
    ) {
        print "Difference found for label '$label':\n";
        print "  $file1:$d1->{line_number} addr1=$d1->{addr1}, addr2=$d1->{addr2}, addr4=$d1->{addr4}, description=\"$d1->{description}\"\n";
        print "  $file2:$d2->{line_number} addr1=$d2->{addr1}, addr2=$d2->{addr2}, addr4=$d2->{addr4}, description=\"$d2->{description}\"\n";
        $diff_count++;
    }
}

if ($diff_count == 0) {
    print "No differences found.\n";
} else {
    print "Total differences found: $diff_count\n";
}
