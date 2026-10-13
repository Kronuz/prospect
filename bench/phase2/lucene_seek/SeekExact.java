// Lucene TermsEnum.seekExact on a vocabulary (one term per line): present and absent
// keys sampled exactly as termdict_bench samples them, median ns over batches of 1024,
// then p50 and p99 of the per-lookup time over batches of 16, each batch timed alone
// less the timer's median cost, as termdict_bench does.
//   java -cp <lucene jars>:. SeekExact VOCAB INDEX_DIR
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import org.apache.lucene.analysis.core.KeywordAnalyzer;
import org.apache.lucene.document.*;
import org.apache.lucene.index.*;
import org.apache.lucene.store.*;
import org.apache.lucene.util.BytesRef;

public class SeekExact {
	public static void main(String[] args) throws Exception {
		List<String> keys = new ArrayList<>(new TreeSet<>(Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8)));
		keys.removeIf(String::isEmpty);
		Path dir = Paths.get(args[1]);
		if (!Files.exists(dir)) {
			IndexWriterConfig cfg = new IndexWriterConfig(new KeywordAnalyzer()).setRAMBufferSizeMB(2048);
			try (IndexWriter w = new IndexWriter(FSDirectory.open(dir), cfg)) {
				for (String k : keys) {
					Document d = new Document();
					d.add(new StringField("t", k, Field.Store.NO));
					w.addDocument(d);
				}
				w.forceMerge(1);
			}
		}
		try (DirectoryReader r = DirectoryReader.open(FSDirectory.open(dir))) {
			TermsEnum te = r.leaves().get(0).reader().terms("t").iterator();
			Random rng = new Random(5);
			List<BytesRef> present = new ArrayList<>(), absent = new ArrayList<>();
			List<String> shuffled = new ArrayList<>(keys);
			Collections.shuffle(shuffled, rng);
			if (shuffled.size() > 1500 * 1024) { shuffled = shuffled.subList(0, 1500 * 1024); }
			for (String k : shuffled) {
				present.add(new BytesRef(k));
				byte[] b = k.getBytes(StandardCharsets.UTF_8);
				byte[] a;
				if (rng.nextBoolean()) { a = Arrays.copyOf(b, b.length + 1); a[b.length] = 'q'; } else { a = b.clone(); a[a.length - 1] ^= 1; }
				BytesRef ar = new BytesRef(a);
				if (!te.seekExact(ar)) { absent.add(ar); }
			}
			System.out.printf("%d keys; Lucene %s, terms index on heap%n", keys.size(), org.apache.lucene.util.Version.LATEST);
			for (int round = 0; round < 4; ++round) {
				System.out.printf("present %8.1f ns   absent %8.1f ns%n", median(te, present), median(te, absent));
			}
			for (int round = 0; round < 2; ++round) {
				double[] p = percentiles(te, present), a = percentiles(te, absent);
				System.out.printf("batch16: present p50 %.0f p99 %.0f ns   absent p50 %.0f p99 %.0f ns%n", p[0], p[1], a[0], a[1]);
			}
		}
	}

	static double[] percentiles(TermsEnum te, List<BytesRef> qs) throws Exception {
		long[] empty = new long[100000];
		for (int i = 0; i < empty.length; ++i) { long t = System.nanoTime(); empty[i] = System.nanoTime() - t; }
		Arrays.sort(empty);
		double overhead = empty[empty.length / 2];
		double[] t = new double[qs.size() / 16];
		long sink = 0;
		for (int b = 0; b < t.length; ++b) {
			long a = System.nanoTime();
			for (int i = b * 16; i < b * 16 + 16; ++i) { if (te.seekExact(qs.get(i))) { ++sink; } }
			t[b] = (System.nanoTime() - a - overhead) / 16.0;
		}
		if (sink == 42) { System.out.print(""); }
		Arrays.sort(t);
		return new double[] {t[(int) (0.50 * (t.length - 1))], t[(int) (0.99 * (t.length - 1))]};
	}

	static double median(TermsEnum te, List<BytesRef> qs) throws Exception {
		List<Double> batches = new ArrayList<>();
		long sink = 0;
		for (int start = 0; start + 1024 <= qs.size() && batches.size() < 1500; start += 1024) {
			long t = System.nanoTime();
			for (int i = start; i < start + 1024; ++i) { if (te.seekExact(qs.get(i))) { ++sink; } }
			batches.add((System.nanoTime() - t) / 1024.0);
		}
		if (sink == 42) { System.out.print(""); }
		Collections.sort(batches);
		return batches.get(batches.size() / 2);
	}
}
