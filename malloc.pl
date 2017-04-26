#!/usr/bin/perl -w
use strict;
use warnings;

use Data::Dumper;

my %set;
while (<>) {
    next if /^Time |^Pid:|^Commit:|^Minimum |^ *set |^ *\d+ s,|^Final memory /;
    if (my ($addr, $size) = /^Thread \d+: Create BoardSubSet (0x[0-9a-f]+): size (\d+), \d+ left$/) {
        die "Duplicate alloc" if $set{$addr};
        $set{$addr} = $size;
        next;
    }
    if (my ($old_addr, $old_size, $new_addr, $new_size) = /^Thread \d+: Convert BoardSubSet (0x[0-9a-f]+) \(size (\d+)\) to red -> (0x[0-9a-f]+) \(size (\d+)\)/) {
        my $old = delete $set{$old_addr} // die "Unknown old address $old_addr";
        $old == $old_size || die "Inconsistent old size";
        die "Duplicate alloc" if $set{$new_addr};
        $set{$new_addr} = $new_size;
        next;
    }
    if (my ($old_addr, $old_size) = /^Thread \d+: Destroy BoardSubSet (0x[0-9a-f]+): size (\d+)$/) {
        my $old = delete $set{$old_addr} // die "Unknown old address $old_addr";
        $old == $old_size || die "Inconsistent old size";
        next;
    }
    if (my ($new_addr, $new_size) = /^Thread \d+: Extract BoardSubSetRed (0x[0-9a-f]+): size (\d+)$/) {
        die "Duplicate alloc" if $set{$new_addr};
        $set{$new_addr} = $new_size;
        next;
    }
    if (my ($old_addr, $new_addr, $new_size) = /^Thread \d+: Resize BoardSubSet (0x[0-9a-f]+) -> (0x[0-9a-f]+): (\d+)$/) {
        my $old_size = $new_size / 2;
        my $old = delete $set{$old_addr} // die "Unknown old address $old_addr";
        $old == $old_size || die "Inconsistent old size";
        die "Duplicate alloc" if $set{$new_addr};
        $set{$new_addr} = $new_size;
        next;
    }
    die "Unknown line $_";
}

print Dumper(\%set);
