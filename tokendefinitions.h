#ifndef __TOKENDEFINITIONS_H
#define __TOKENDEFINITIONS_H

enum class eViews {
	rootView,
	detailView
};

enum class eViewElementsRoot {
	background,
	header,
	footer,
	infopane,
	watch,
	message,
	scrollbar
};

enum class eViewGrids {
	cover,
	detail,
	list
};

enum class eViewElementsDetail {
	background,
	header,
	footer
};

enum class eTokenGridInt {
	viewmode = 0,
	viewgroup,
	viewCount,
	viewoffset,
	viewoffsetpercent,
	duration,
	year,
	hasthumb,
	hasart,
	ismovie,
	isepisode,
	isdirectory,
	isshow,
	isseason,
	originallyAvailableYear,
	originallyAvailableMonth,
	originallyAvailableDay,
	season,
	episode,
	leafCount,
	viewedLeafCount,
	childCount,
	rating,
	hasseriesthumb,
	hasbanner,
	columns,
	rows,
	position,
	totalcount,
	bitrate,
	width,
	height,
	audioChannels,
	isdummy,
	isserver,
	serverport
};

enum class eTokenGridStr {
	title = 0,
	orginaltitle,
	summary,
	contentrating,
	ratingstring,
	studio,
	thumb,
	art,
	seriestitle,
	seriesthumb,
	banner,
	videoResolution,
	aspectRatio,
	audioCodec,
	videoCodec,
	container,
	videoFrameRate,
	serverstartpointname,
	serverip,
	serverversion,
	tabname
};

enum class eTokenGridActorLst {
	roles = 0
};

enum class eTokenGridGenresLst {
	genres = 0
};
	
enum class eTokenMessageInt {
	displaymessage = 0
};

enum class eTokenMessageStr {
	message = 0
};

enum class eTokenScrollbarInt {
	height = 0,
	offset
};

enum class eTokenBackgroundInt {
	viewmode = 0,
	isdirectory
};

enum class eTokenBackgroundStr {
	selecteditembackground = 0,
	currentdirectorybackground
};

enum class eTokenTimeInt {
	sec = 0,
	min,
	hour,
	hmins,
	day,
	year
};

enum class eTokenTimeStr {
	time = 0,
	dayname,
	daynameshort,
	dayleadingzero,
	month,
	monthname,
	monthnameshort
};

enum class eTokenFooterInt {
	red1 = 0,
	red2,
	red3,
	red4,
	green1,
	green2,
	green3,
	green4,
	yellow1,
	yellow2,
	yellow3,
	yellow4,
	blue1,
	blue2,
	blue3,
	blue4
};

enum class eTokenFooterStr {
	red = 0,
	green,
	yellow,
	blue
};


#endif //__TOKENDEFINITIONS_H