#!/usr/bin/perl
# survey: which .ini keys do the shipped assets use that Templates.cpp never reads?
use strict; use warnings; use File::Find;

my $asset_root = 'C:/Users/Administrator/Downloads/work2/RP/assets';
my $tpl_src    = 'C:/Users/Administrator/Downloads/work2/RP/src/Templates.cpp';

my %used;                      # lowercased key -> read by RP
{
    open my $fh, '<', $tpl_src or die "Templates.cpp: $!";
    while (my $l = <$fh>) {
        while ($l =~ /(?:getNum|getBool|get|has|geti)\s*\(\s*"([a-z0-9_]+)"/g) { $used{$1} = 1; }
    }
    close $fh;
}
# keys built from variables (action_/turret_/leg_) are handled by prefix matching below
my @prefixes = qw(action_ turret_ leg_ effect_ attachment_);

my %count;                     # "section|key" -> number of ini files using it
my @files;
find(sub { push @files, $File::Find::name if /\.ini$/i && !/\/ALL-/ }, $asset_root);
for my $f (@files) {
    open my $fh, '<', $f or next;
    my ($cur, %seen) = (undef, ());
    while (my $l = <$fh>) {
        $l =~ s/^\s+|\s+$//g;
        next if $l eq '' or $l =~ /^#/;
        if ($l =~ /^\[(.+?)\]$/) { $cur = lc $1; $cur =~ s/\s//g; next }
        next unless defined $cur;
        my $colon = index($l, ':'); my $eq = index($l, '=');
        my $p = ($colon >= 0 && ($eq < 0 || $colon < $eq)) ? $colon : $eq;
        next if $p < 0;
        my $k = lc substr($l, 0, $p); my $v = substr($l, $p + 1);
        $k =~ s/\s//g; $v =~ s/^\s+//;
        next if $k eq '' or $v eq '' or $v =~ /^#/;
        my $id = "$cur|$k";
        next if $seen{$id}++;
        $count{$id} //= 0;
        $count{$id}++;
    }
    close $fh;
}

my %miss;
for my $id (keys %count) {
    my ($sec, $k) = split /\|/, $id, 2;
    my $base = $k;
    $base =~ s/_\d+_/_/g;        # turret_1_foo -> turret__foo style normalisation
    my $skip = 0;
    for my $p (@prefixes) { $skip = 1 if $sec =~ /^\Q$p\E/ || $k =~ /^$p/; }
    next if $skip;
    next if $used{$k} || $used{$base};
    $miss{$id} = $count{$id};
}

printf("%-34s %-32s %s\n", 'section', 'key', 'files');
for my $id (sort { $miss{$b} <=> $miss{$a} } keys %miss) {
    my ($sec, $k) = split /\|/, $id, 2;
    printf("%-34s %-32s %d\n", $sec, $k, $miss{$id});
}
printf("total unread key/section pairs: %d\n", scalar keys %miss);
