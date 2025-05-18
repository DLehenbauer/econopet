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

        print "  $file1:$d1->{line_number} Extra label '$label' ($d1->{addr1}, $d1->{addr2}, $d1->{addr4}, $d1->{description})\n";

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
            foreach my $match_label (@matching_labels) {
                my $d2 = ${$data2}{$match_label};
                print "    $file2:$d2->{line_number} matches? '$match_label' ($d2->{addr1}, $d2->{addr2}, $d2->{addr4}, $d2->{description})\n";
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
    return ($addr1 eq "") || ($addr1 eq $addr2);
}

# Validate address fields in the data
sub validate_addresses {
    my ($file, $data) = @_;
    my $error_count = 0;

    foreach my $label (keys %{$data}) {
        my $d = $data->{$label};
        foreach my $addr_field (qw(addr1 addr2 addr4)) {
            my $addr = $d->{$addr_field};

            # Skip empty strings
            next if $addr eq "";

            # Check for valid hexadecimal, "-", or "?"
            unless ($addr =~ /^[0-9a-fA-F]+$/ || $addr eq "-" || $addr eq "----" || $addr eq "?") {
                print "ERROR: $file:$d->{line_number} Label '$label' $addr_field '$addr' is not a valid hexadecimal value, '-', or '?'\n";
                $error_count++;
            }
        }
    }
    return $error_count;
}

# Load the two CSV files
my $file1 = "memorymap-ocr.csv";
my $file2 = "memorymap-gen.csv";

my %data1 = load_csv($file1);
my %data2 = load_csv($file2);

# Validate addresses in both data sets
my $total_error_count = 0;
$total_error_count += validate_addresses($file1, \%data1);
$total_error_count += validate_addresses($file2, \%data2);

if ($total_error_count > 0) {
    print "Total errors found in address fields: $total_error_count\n";
    exit 1;
}

my $diff_count = 0;

# Check for labels present in data1 but not in data2
print "\nLabels in $file1 but not in $file2:\n";
$diff_count += check_missing_labels(\%data1, \%data2, $file1, $file2, $diff_count);

# Check for labels present in data2 but not in data1
print "\nLabels in $file2 but not in $file1:\n";
$diff_count += check_missing_labels(\%data2, \%data1, $file2, $file1, $diff_count);

# Compare the data
print "\nLabels in both $file1 and $file2 with discrepencies:\n";

# Collect common labels and their data
my @common_labels;
foreach my $label (keys %data1) {
    if (exists $data2{$label}) {
        push @common_labels, { label => $label, d1 => $data1{$label}, d2 => $data2{$label} };
    }
}

# Sort common labels by line number in file1
@common_labels = sort { $a->{d1}->{line_number} <=> $b->{d1}->{line_number} } @common_labels;

# Check for differences in common labels
my $max_file_info_length = 25;
foreach my $common_label_ref (@common_labels) {
    my $label = $common_label_ref->{label};
    my $d1 = $common_label_ref->{d1};
    my $d2 = $common_label_ref->{d2};

    if (!are_addresses_equal($d1->{addr1}, $d2->{addr1}) ||
        !are_addresses_equal($d1->{addr2}, $d2->{addr2}) ||
        !are_addresses_equal($d1->{addr4}, $d2->{addr4}) ||
        ($d1->{description} ne $d2->{description})
    ) {
        # Format the output with padding for alignment
        my $file1_info = sprintf "%s:%s", $file1, $d1->{line_number};
        my $file2_info = sprintf "%s:%s", $file2, $d2->{line_number};

        print "  Difference found for label '$label':\n";
        printf "    %-${max_file_info_length}s addr1=%-4s, addr2=%-4s, addr4=%-4s, description=\"%s\"\n",
            $file1_info, $d1->{addr1}, $d1->{addr2}, $d1->{addr4}, $d1->{description};
        printf "    %-${max_file_info_length}s addr1=%-4s, addr2=%-4s, addr4=%-4s, description=\"%s\"\n",
            $file2_info, $d2->{addr1}, $d2->{addr2}, $d2->{addr4}, $d2->{description};

        $diff_count++;
    }
}

if ($diff_count == 0) {
    print "No differences found.\n";
} else {
    print "Total differences found: $diff_count\n";
}
