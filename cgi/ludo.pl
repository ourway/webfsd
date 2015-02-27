#!/usr/bin/perl -wU
sleep(5);
print "Content-Type: text/html\nStatus: 200 OK\nCache-Control:
no-store\nPragma: no-cache\nConnection: close\n\n";

#The next line seems to make it a lot worse, but also without it it goes
wrong.
open (STDERR, ">&STDOUT");


sleep(5);
print "<HTML><body> Test<br><br> Test2</body></html>\n";

