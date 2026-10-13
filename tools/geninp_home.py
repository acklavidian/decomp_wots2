import sys
N=int(sys.argv[1]); atk=len(sys.argv)>2
out=["340:start","500:cross","560:cross","700:down","710:down","720:down","730:down","740:down","770:cross"]
f=1200
while f<N:
    for c in range(f+60,f+290,40): out.append(f"{c}:cross")
    if atk:
        for c in range(f+70,f+290,20): out.append(f"{c}:square")
    f+=300
print(",".join(out))
