#!/usr/bin/perl
# survey: does RWX's name-based turret/projectile keying match the shipped assets?
use strict; use warnings; use File::Find;
my $root = 'C:/Users/Administrator/Downloads/work2/RP/assets';
my @files;
find(sub { push @files, $File::Find::name if /\.ini$/i }, $root);

my (@badProj, @badCopy, @namedOnly, @orderGap);
for my $f (@files) {
    open my $fh, '<', $f or next;
    my (@proj, @tur, %turKeys, $cur);
    while (my $l = <$fh>) {
        $l =~ s/^\s+|\s+$//g;
        next if $l eq '' or $l =~ /^#/;
        if ($l =~ /^\[(.+?)\]$/) { $cur = lc $1; next }
        next unless defined $cur;
        my ($k, $v);
        { my $colon = index($l, ':'); my $eq = index($l, '=');
          my $p = $colon >= 0 && ($eq < 0 || $colon < $eq) ? $colon : $eq;
          next if $p < 0;
          ($k, $v) = (substr($l, 0, $p), substr($l, $p + 1));
          $k =~ s/^\s+|\s+$//g; $v =~ s/^\s+|\s+$//g; }
        if ($cur =~ /^projectile_(.+)$/) { push @proj, $1 unless grep { $_ eq $1 } @proj }
        if ($cur =~ /^turret_(.+)$/) {
            push @tur, $1 unless grep { $_ eq $1 } @tur;
            $turKeys{$cur}{$k} = $v;
        }
    }
    close $fh;
    next unless @proj or @tur;
    my $rel = $f; $rel =~ s{^\Q$root\E/}{};
    my @numeric = grep { /^\d+$/ } @proj;
    push @namedOnly, "$rel  proj=@proj" if @proj and @numeric != @proj;
    for my $t (@tur) {
        my $keys = $turKeys{"turret_$t"} or next;
        if (defined $keys->{projectile}) {
            my $ref = lc $keys->{projectile}; $ref =~ s/\s//g;
            my @hit = grep { lc($_) eq $ref } @proj;
            push @badProj, "$rel [turret_$t] projectile:$keys->{projectile}  have:@proj" unless @hit;
        }
        if (defined $keys->{copyfrom}) {
            my $ref = lc $keys->{copyfrom}; $ref =~ s/\s//g;
            my @hit = grep { lc($_) eq $ref } @tur;
            push @badCopy, "$rel [turret_$t] copyFrom:$keys->{copyfrom}  have:@tur" unless @hit;
        }
    }
    # numeric names that are not 1..n in file order: index-by-order would differ from index-by-number
    if (@numeric == @proj and @proj) {
        my $i = 0; my $ok = 1;
        for my $p (@proj) { $i++; $ok = 0 if $p != $i }
        push @orderGap, "$rel  proj=@proj" unless $ok;
    }
}
print "files scanned: ", scalar(@files), "\n";
print "--- [turret_*] projectile refs with no matching [projectile_<name>] (", scalar @badProj, ") ---\n", @badProj ? join("\n", @badProj) : "(none)", "\n";
print "--- turret copyFrom with no matching [turret_<name>] (", scalar @badCopy, ") ---\n", @badCopy ? join("\n", @badCopy) : "(none)", "\n";
print "--- files with named (non numeric) projectile sections (", scalar @namedOnly, ") ---\n", @namedOnly ? join("\n", @namedOnly) : "(none)", "\n";
print "--- numeric projectile sections not in 1..n order (", scalar @orderGap, ") ---\n", @orderGap ? join("\n", @orderGap) : "(none)", "\n";
