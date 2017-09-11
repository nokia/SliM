set term svg size 500,200
set autoscale yfixmax

# === Variables to plot"

nf=system("echo $NF")
nosnapsz=system("echo $NOSNAPSZ")
ploss_log=system("echo $PLOSS_LOG")
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

ustotime(t) = t/1000000.0

set xlabel "Applied dataplane workload (Mbit/s)"
#set logscale x

if (ploss_log != 0) {
	set key bottom right
	set logscale y
	set format y '%2.4fs'
	set yrange [0.0001:4.5]
} else {
	set key top left
	set format y '%2.1fs'
	set yrange [0:3]
}
M=1000000
G=1000*M
set xrange [0:1000]
set format x "%.0s%c"

set ylabel "(Lost packets) / (pkts/s)"

set style line 1 lt 1 pt 2 lc rgb "dark-green" lw 1
set style line 2 lt 2 pt 4 lc rgb "dark-orange" lw 1
set style line 3 lt 3 pt 1 lc rgb "blue" lw 1
set style line 4 lt 4 pt 6 lc rgb "red" lw 1

set ticslevel 0
set grid x
set grid y


mkval(x) = x

plot inFileDefDLB using 1:(mkval($26)):(mkval($26+$27)):(mkval($26-$27)) w yerrorlines title " " . nf . " with SliM (1400)" linestyle 1, \
	inFileLgcyDLB using 1:(mkval($26)):(mkval($26+$27)):(mkval($26-$27)) w yerrorlines title "Duplication (1400)" linestyle 2, \
	inFileDefDL using 1:(mkval($26)):(mkval($26+$27)):(mkval($26-$27)) w yerrorlines title " " . nf . " with SliM (512)" linestyle 3, \
	inFileLgcyDL using 1:(mkval($26)):(mkval($26+$27)):(mkval($26-$27)) w yerrorlines title "Duplication (512)" linestyle 4



