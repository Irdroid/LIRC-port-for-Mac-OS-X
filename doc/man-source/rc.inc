[NAME]
rc - basic LIRC program to send infra-red commands

[DESCRIPTION]
Asks the \fBlircd\fR daemon to send one or more CIR
(Consumer Infra-Red) commands. This is intended for remote control
of electronic devices such as TV boxes, HiFi sets, etc.
.PP
\fIDIRECTIVE\fR can be:
.nf
.RS 3
\fBSEND_ONCE\fR    - send \fICODE\fR [\fICODE\fR ...] once
\fBSEND_START\fR   - start repeating \fICODE\fR
\fBSEND_STOP\fR    - stop repeating \fICODE\fR
\fBLIST\fR         - list configured remote items
.RE
.fi

.TP 
\fIREMOTE\fR is the name of a remote, as described in the \fBlircd\fR
configuration file.

.TP
\fICODE\fR is the name of a remote control key of \fIREMOTE\fR, as it
appears in the \fBlircd\fR configuration file.

.PP
For the \fBLIST\fR \fIDIRECTIVE\fR, \fIREMOTE\fR and/or \fICODE\fR
can be empty:

.nf
.RS 3
\fBLIST\fR   ""    ""   - list all configured remote names
\fBLIST\fR \fIREMOTE\fR  ""   - list all codes of \fIREMOTE\fR
\fBLIST\fR \fIREMOTE\fR \fICODE\fR  - list only \fICODE\fR of \fIREMOTE\fR
.RE
.fi

[EXAMPLES]
.nf
.RS 3
rc LIST DenonTuner ""
rc SEND_ONCE  DenonTuner PROG-SCAN 
rc SEND_ONCE  OnkyoAmpli VOL-UP VOL-UP VOL-UP VOL-UP
rc SEND_START OnkyoAmpli VOL-DOWN ; sleep 3
rc SEND_STOP  OnkyoAmpli VOL-DOWN
.RE
.fi
[FILES]
.TP
.B /etc/lircd.conf
Default \fBlircd\fR configuration file.  It should contain all the
remotes, their infra-red codes and the corresponding timing and 
waveform details.

.SH DIAGNOSTICS
If \fBlircd\fR is not running (or /dev/lircd lacks write permissions)
\fBrc\fR aborts with the following diagnostics:
.nf
"rc: could not connect to socket"
"rc: Connection refused" (or "Permission denied").
.fi

[SEE ALSO]
.BR lircd "(8), " mode2 "(1), " smode2 "(1), " xmode2 "(1), " 
.BR irrecord "(1), " irw "(1), " http://www.lirc.org .
