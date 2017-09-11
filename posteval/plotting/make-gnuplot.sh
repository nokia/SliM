export BASEDIR=../../../slim-data

mkdir -p $BASEDIR/outPlotted
mkdir -p $BASEDIR/outPlotted/svg

function makeGPTime {
	export GRT_BW=$1
	export OUTNAME=miscOverTime_${GRT_BW}${GRT_PARAMS}_withploss
	OUT1="$BASEDIR/outPlotted/$OUTNAME"
	OUTSVG1="$BASEDIR/outPlotted/svg/$OUTNAME"
	export WITH_PLOSS="1"
	gnuplot miscOverTime.gnuplot > $OUTSVG1.svg
	inkscape --without-gui --export-pdf=$OUT1.pdf $OUTSVG1.svg
}

export GRT_PARAMS="_dl05"

makeGPTime 100
makeGPTime 200
makeGPTime 300
makeGPTime 400
makeGPTime 500
makeGPTime 600
makeGPTime 700
makeGPTime 800


#export GRT_PARAMS="_dl05_nosnapsz"

#makeGPTime 128
#makeGPTime 64
#makeGPTime 32
#makeGPTime 16
#makeGPTime 8
#makeGPTime 4
#makeGPTime 2
