export BASEDIR=../../../slim-data

mkdir -p $BASEDIR/outPlotted
mkdir -p $BASEDIR/outPlotted/svg


function makeGPVoi {
	OUTPUT="$BASEDIR/outPlotted/$2"
	OUTPUTSVG="$BASEDIR/outPlotted/svg/$2"
	gnuplot $1 > $OUTPUTSVG.svg
	inkscape --without-gui --export-pdf=$OUTPUT.pdf $OUTPUTSVG.svg
}

export NOSNAPSZ=0
makeGPVoi statetraffic_bw.gnuplot 5_statetraffic_bw

makeGPVoi rtt_bw.gnuplot 4_rtt_bw

export PLOSS_LOG=0
makeGPVoi ploss_bw.gnuplot 2_ploss_bw
makeGPVoi sla15_bw.gnuplot 2_sla15_bw
export PLOSS_LOG=1
makeGPVoi ploss_bw.gnuplot 3_ploss_bw_log
makeGPVoi sla15_bw.gnuplot 3_sla15_bw_log

makeGPVoi duration_bw.gnuplot 1_duration

makeGPVoi pktbufsize_bw.gnuplot 6_pktbufsize

#export NOSNAPSZ=1
#makeGPVoi statetraffic_bw.gnuplot statetraffic_bw_nosnapsz
#makeGPVoi rtt_bw.gnuplot rtt_bw_nosnapsz
#makeGPVoi ploss_bw.gnuplot ploss_bw_nosnapsz

