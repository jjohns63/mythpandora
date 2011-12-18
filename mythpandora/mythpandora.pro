include ( ../../mythconfig.mak )
include ( ../../settings.pro )
include ( ../../programs-libs.pro )

QT += xml sql opengl qt3support network

TEMPLATE = lib
CONFIG += plugin thread
TARGET = mythpandora
target.path = $${LIBDIR}/mythtv/plugins
INSTALLS += target

INCLUDEPATH += ../pianobar/src/libpiano
INCLUDEPATH += ../pianobar/src/libwaitress
INCLUDEPATH += ../pianobar/src/libezxml
INCLUDEPATH += $${PREFIX}/include/mythtv
INCLUDEPATH += $${PREFIX}/include/mythtv/libmyth
INCLUDEPATH += $${PREFIX}/include/mythtv/libmythui
INCLUDEPATH += $${PREFIX}/include/mythtv/libmythdb

LIBS += -lmythavformat
LIBS += -lmythavcodec
LIBS += -lmythavcore
LIBS += -lmythavutil
LIBS += -lgnutls

LIBS += -lmad -lfaad

# Input
HEADERS += config.h mythpandora.h player.h
SOURCES += main.cpp player.c mythpandora.cpp

SOURCES += ../pianobar/src/libezxml/ezxml.c
HEADERS += ../pianobar/src/libezxml/ezxml.h

SOURCES += ../pianobar/src/libwaitress/waitress.c
HEADERS += ../pianobar/src/libwaitress/waitress.h

SOURCES  += ../pianobar/src/libpiano/crypt.c ../pianobar/src/libpiano/piano.c ../pianobar/src/libpiano/xml.c
HEADERS  += ../pianobar/src/libpiano/config.h ../pianobar/src/libpiano/crypt_key_output.h ../pianobar/src/libpiano/xml.h ../pianobar/src/libpiano/crypt.h ../pianobar/src/libpiano/piano.h ../pianobar/src/libpiano/crypt_key_input.h ../pianobar/src/libpiano/piano_private.h

include ( ../../libs-targetfix.pro )
