bam: bam.c
	gcc -g3 -I/usr/local/include -L/usr/local/lib -lcmph -lmicrohttpd bam.c -o bam

urls.txt: test.tsv
	cut -f1 $^ | perl -nle 'print "http://localhost:8080/$$_"' > $@

clean:
	rm -rf bam bam.dSYM
