#!/usr/bin/perl -w
use strict;
use warnings;

my $edge;

sub rotate {
    my ($board) = @_;
    my @lines = map [/[OX.]/g], $board =~ /.+/g;
    return join("", map join("", "|", map(shift(@$_). " ",@lines),"|\n"), 1..@lines);
}

sub get_board {
    my ($fh) = @_;
    my $board = "";
    $edge = <$fh>;
    while (<$fh>) {
        return $board if $_ eq $edge;
        $board .= $_;
    }
    die "Board does not end";
}

sub parse {
    my ($file) = @_;

    my %boards;
    my $from;
    open(my $fh, "<", $file) || die "Could not open '$file': $!";
    local $_;
    my $froms    = 0;
    my $inserted = 0;
    while (<$fh>) {
        if (/^  From: /) {
            ++$froms;
            $from = get_board($fh);
        } elsif (/^   Inserted /) {
            ++$inserted;
            my $board1 = get_board($fh);
            my $board2 = rotate($board1);
            if ($board1 le $board2) {
                $boards{$board1} = $from;
            } else {
                $boards{$board2} = rotate($from);
            }
        }
    }
    print "From: $froms, inserted: $inserted\n";
    return \%boards;
}

sub show_diff {
    my ($text, $first, $second) = @_;
    my %boards;
    @boards{keys %$first} = ();
    print "Before: " . keys %boards, "\n";
    delete @boards{keys %$second};
    print "After: " . keys %boards, "\n";
    if (%boards) {
        print "$text:\n";
        for my $board (sort keys %boards) {
            print "Ancestor:\n";
            print $edge, $first->{$board}, $edge;
            print "Board:\n";
            print $edge, $board, $edge;
            print "\n";
            print "Ancestor rotated:\n";
            print $edge, rotate($first->{$board}), $edge;
            print "Board rotated:\n";
            print $edge, rotate($board), $edge;
            print "\n";
            print "====================================\n";
        }
    }
}

my $file1 = shift || die "Usage $0 file1 file2";
my $file2 = shift || die "Usage $0 file1 file2";
die "Usage $0 file1 file2" if @ARGV;

my $boards1 = parse($file1);
my $boards2 = parse($file2);

show_diff("Only in $file1", $boards1, $boards2);
show_diff("Only in $file2", $boards2, $boards1);
