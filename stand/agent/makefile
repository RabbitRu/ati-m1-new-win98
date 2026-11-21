# Open Watcom: run "wmake" in this directory (WATCOM and INCLUDE must be set).
CFLAGS = -q -bt=nt -3r -os -wx -zq

w98agent.exe: w98agent.c
	wcl386 $(CFLAGS) -l=nt_win -fe=w98agent.exe w98agent.c wsock32.lib

clean: .SYMBOLIC
	rm -f *.obj *.exe *.err *.map
