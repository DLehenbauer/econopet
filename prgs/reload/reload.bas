if peek(142)<>42 then poke 140,0: poke 141,0: poke 142,42
print "loading iteration:";
h% = peek(141)
l% = peek(140)
print h%*256+l%
l% = l%+1
if l%>255 then h% = h%+1: l% = 0
poke 140,l%
poke 141,h%
load "reload"
