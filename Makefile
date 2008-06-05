# Makefile for a FEA-Finite Strains project

BOOSTPATH = ../../../boost_1_35_0
LUAPATH = ../../../lua-5.1.3
INCLUDE = -I $(BOOSTPATH) -I $(LUAPATH)/src
LINK = g++
CPP = g++
#LINK = /opt/local/bin/g++-mp-4.2
#CPP = /opt/local/bin/g++-mp-4.2
OUTPUT = finitestrain
SOURCES = $(wildcard *.cpp)
HEADERS = $(wildcard *.h)
OBJECTS = $(patsubst %.cpp,%.o,$(wildcard *.cpp))
DEPENDENCIES = $(patsubst %.cpp,%.d,$(wildcard *.cpp))

PRECOMPILEDHEADER = std.h.gch

LINKFLAGS = -lm -llua -L$(LUAPATH)/src
#CPPFLAGS = -Wno-deprecated -pg
CPPFLAGS = -Wall -Wno-deprecated -O3 -fast

all: $(OUTPUT) 

%.d: %.cpp 
	@set -e; rm -f $@; \
	$(CPP) -MM $(CPPFLAGS) $(INCLUDE) $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

.cpp.o .d: 
	$(CPP) -c $(CPPFLAGS) $(INCLUDE) -o "$@" "$<"


#$(PRECOMPILEDHEADER): std.h
#	$(CPP) $(CPPFLAGS) -I $(INCLUDE) std.h


-include $(SOURCES:.cpp=.d)


$(OUTPUT): $(OBJECTS) #$(PRECOMPILEDHEADER) $(OBJECTS) 
	$(LINK) $(CPPFLAGS) $(LINKFLAGS) $(OBJECTS) -o $(OUTPUT) 

todo:
	grep -R -n "TODO" .


.PHONY : clean depclean reallyclean todo


clean:
	-rm $(OBJECTS) $(OUTPUT) 

depclean:
	-rm $(DEPENDENCIES) 


reallyclean: clean depclean 
	-rm $(DEPENDENCIES) #$(PRECOMPILEDHEADER)
