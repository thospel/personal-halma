#!/usr/bin/perl -w

# Process output of the halma -R option
use strict;
use warnings;

use Data::Dumper;
$Data::Dumper::Indent = 1;
$Data::Dumper::Sortkeys = 1;

my %count;
my @history = map [0, 0], 0..3;
while(<>) {
    my ($val, $sym) = /^([0-9a-f]+)([+-])$/ or die "Cannot parse $_";
    $val = hex($val);
    $sym = $sym eq "+" ? 0 : 1;
    for my $h (1..@history) {
        my $diff = $val - $history[-$h][0];
        if (abs($diff) <= 7) {
            my $asym = $sym eq $history[-$h][1] ? "+" : "-";
            ++$count{"$h:$diff$asym"};
            splice(@history, -$h, 1);
            goto FOUND;
        }
    }
    my $asym = $sym eq $history[-1][1] ? "+" : "-";
    ++$count{"reset$asym"};
    shift @history;
  FOUND:
    push @history, [$val, $sym];
}

my $total = 0;
$total += $_ for values %count;
my %percount;
while (my ($n, $c) = each %count) {
    my $val = int(10000*$c / $total) / 100;;
    $percount{$n} = $val if $val;
}
print Dumper(\%percount);

huffman(\%percount);

sub huffman {
    my ($h) = @_;

    my @work;

    while (my ($k, $v) = each %$h) {
        push @work, [$k, $v];
    }
    my @names = sort { $b->[1] <=> $a->[1]} @work;
    my $name = "A";
    my %table;
    while (@work > 1) {
        @work = sort { $a->[1] <=> $b->[1]} @work;
        my $left  = shift @work;
        my $right = shift @work;
        $table{$left ->[0]} = [$name, 0];
        $table{$right->[0]} = [$name, 1];
        push @work, [$name++, $left ->[1] + $right->[1]];
    }
    for my $name (@names) {
        my $n = $name->[0];
        my $bits = "";
        while (my $parent = $table{$n}) {
            $bits = $parent->[1] . $bits;
            $n = $parent->[0];
        }
        print "$name->[0]:\t$bits\n";
    }
}
