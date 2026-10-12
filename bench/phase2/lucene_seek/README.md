# Lucene seekExact baseline

`SeekExact.java` indexes a vocabulary (one term per line, one document per term) with
Lucene and times `TermsEnum.seekExact` on present keys and on absent keys made by
appending a byte or changing the last byte, sampled exactly as `termdict_bench` samples
them.

```sh
L=search-benchmark-game/engines/lucene-10.4.0/build/dependencies
javac -cp "$L/*" SeekExact.java
java -Xmx6g -cp "$L/*:." SeekExact vocab.txt idx
```

Wikipedia vocabulary (3,964,752 terms), gate VM, Lucene 10.4.0: present 1,170–1,220 ns,
absent 1,190–1,300 ns. `termdict` on the same terms in the same VM: present 614–676 ns,
absent 677–696 ns (0.54× Lucene).
