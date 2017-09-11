set term svg size 500,220
set autoscale yfixmax

# === Variables to plot"

nosnapsz=system("echo $NOSNAPSZ")
basedir=system("echo $BASEDIR")

# ===

inDir = basedir . "/outPlottable"

if (nosnapsz == 0) {
	inFileDefDLB = inDir . "/voi_default_bws_pkt1400_dl05.csv
	inFileLgcyDLB = inDir . "/voi_legacy_bws_pkt1400_dl05.csv
	inFileDefDL = inDir . "/voi_default_bws_pkt512_dl05.csv
	inFileLgcyDL = inDir . "/voi_legacy_bws_pkt512_dl05.csv
} else {
	inFileDefDL = inDir . "/voi_default_bws_pkt512_dl05_nosnapsz.csv
	inFileLgcyDL = inDir . "/voi_legacy_bws_pkt512_dl05_nosnapsz.csv
}

set xlabel "Applied dataplane workload (bit/s)"
#set logscale x
M=1000000
G=1000*M

set xrange [0:1000]
set format y "%.0s%c"
set format x "%.0s%c"

set ylabel "State Transfer Volume (bytes)"
set yrange [0:200*M]


set style line 1 lt 1 pt 2 lc rgb "dark-green" lw 1
set style line 2 lt 2 pt 4 lc rgb "dark-orange" lw 1
set style line 3 lt 3 pt 1 lc rgb "blue" lw 1
set style line 4 lt 4 pt 6 lc rgb "red" lw 1

set ticslevel 0
set grid x
set grid y
set key top left

plot inFileDefDLB using 1:2:($2+$3):($2-$3) w yerrorlines title "SliM (1400)" linestyle 1, \
	inFileLgcyDLB using 1:2:($2+$3):($2-$3) w yerrorlines title "Duplication (1400)" linestyle 2, \
	inFileDefDL using 1:2:($2+$3):($2-$3) w yerrorlines title "SliM (512)" linestyle 3, \
	inFileLgcyDL using 1:2:($2+$3):($2-$3) w yerrorlines title "Duplication (512)" linestyle 4


