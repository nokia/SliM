

# === Variables to plot

inDir = "../output/plottable"

bw=system("echo $GRT_BW")
grtparams=system("echo $GRT_PARAMS")
withploss=system("echo $WITH_PLOSS")
outname=system("echo $OUTNAME")

withtitle=1

# ===

if (withploss > 0) {
	set term svg size 500,500
} else {
	set term svg size 500,350
}

set autoscale yfixmax

bw_eff = 768*1./bw*1000000*8*128*2
bw_eff_m = bw_eff/1000/1000

set datafile missing "NaN"

set label 1 at 0.5, 0.5
#set label 1 "Blargh" tc lt 3

set ylabel "bytes"
set xlabel "Time"
set format x "%.0fs"
set format y "%.0s%c"
#set xtics 0,1
set xrange [2:13]
set grid x

set key left

#unset ytics

set yrange [0:*]

#set style line 1 lt 1 lc rgb "blue" lw 2
#set style line 2 lt 2 lc rgb "dark-green" lw 2
#set style line 3 lt 3 lc rgb "dark-yellow" lw 2
#set style line 4 lt 4 lc rgb "red" lw 2

#set style line 5 lt 5 lc rgb "dark-grey" lw 1
#set style line 6 lt 6 lc rgb "dark-grey" lw 1

if (withtitle > 0) {
	set multiplot title sprintf("%s, %.0f Mbps", outname, bw_eff_m);
} else {
	set multiplot
}

FILE=inDir . "/results_time_default_bw" . bw . grtparams . ".csv"
FILE_COMPARE=inDir . "/results_time_legacy_bw" . bw . grtparams . ".csv"

set origin 0, 0

if (withploss > 0) {
	set size 1, 0.38 #With PLoss
} else {
	set size 1, 0.5
}



set lmargin 10
set rmargin 2
set tmargin 0.1

plot FILE using 1:2:($2-$3):($2+$3) ls 1 title "SliM" with yerrorlines, \
	FILE_COMPARE using 1:2:($2-$3):($2+$3) ls 2 title "Dup." with yerrorlines


unset xlabel
set format x ""
set bmargin 0.1
set tmargin 0.1


if (withploss > 0) {
	set origin 0,0.4 #With PLoss
} else {
	set origin 0,0.55
}


set size 1, 0.23

set ylabel "RTT (ms)"
set ytics
set yrange [5:35]
set format y "%.1f"

nstotime(t) = t/1000000.0

plot FILE using 1:(nstotime($8)):(nstotime($8-$9)):(nstotime($8+$9)) ls 1 title "SliM" with yerrorlines, \
	FILE_COMPARE using 1:(nstotime($8)):(nstotime($8-$9)):(nstotime($8+$9)) ls 2 title "Rep." with yerrorlines, \

if (withploss <= 0) exit

set origin 0,0.65
if (withtitle > 0) {
	set size 1,0.38
} else {
	set size 1,0.42
}

#set logscale y
set yrange [0:100]

set ylabel "Packet Loss"
set format y '%2.2f%%'

set tmargin 3
#set key out horiz right top

topercent(x) = x*100

# For cumulative bandwidths:
#plot FILE using 1:14:($14-$15):($14+$15) ls 1 title "SliM Src." with yerrorlines, \
#	FILE using 1:17:($17-$18):($17+$18) ls 2 title "SliM Dest." with yerrorlines, \
#	FILE_COMPARE using 1:14:($14-$15):($14+$15) ls 3 title "Legacy Src." with yerrorlines, \
#	FILE_COMPARE using 1:17:($17-$18):($17+$18) ls 4 title "Legacy Dst." with yerrorlines

plot FILE_COMPARE using 1:(topercent($11)):(topercent($11-$12)):(topercent($11+$12)) ls 2 title "Rep." with yerrorlines, \
	FILE using 1:(topercent($11)):(topercent($11-$12)):(topercent($11+$12)) ls 1 title "SliM" with yerrorlines
	














