import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

import org.apache.lucene.analysis.CharArraySet;
import org.apache.lucene.analysis.standard.StandardAnalyzer;
import org.apache.lucene.document.Document;
import org.apache.lucene.document.Field;
import org.apache.lucene.document.StringField;
import org.apache.lucene.document.TextField;
import org.apache.lucene.index.DirectoryReader;
import org.apache.lucene.index.IndexWriter;
import org.apache.lucene.index.IndexWriterConfig;
import org.apache.lucene.index.IndexWriterConfig.OpenMode;
import org.apache.lucene.index.Term;
import org.apache.lucene.queryparser.classic.QueryParser;
import org.apache.lucene.search.IndexSearcher;
import org.apache.lucene.search.Query;
import org.apache.lucene.search.TopScoreDocCollectorManager;
import org.apache.lucene.search.similarities.BM25Similarity;
import org.apache.lucene.store.FSDirectory;

// strata's soak op stream (bench/soak/soak.py) through Lucene: upserts and deletes by a
// string id, commits (with a near-real-time reader reopened from the writer), and queries.
public class Churn {
    public static void main(String[] args) throws Exception {
        final Path dir = Paths.get(args[0]);
        Files.createDirectories(dir);
        final StandardAnalyzer analyzer = new StandardAnalyzer(CharArraySet.EMPTY_SET);
        final IndexWriterConfig config = new IndexWriterConfig(analyzer).setOpenMode(OpenMode.CREATE);
        final PrintStream out = new PrintStream(System.out, false);
        try (FSDirectory directory = FSDirectory.open(dir);
                IndexWriter writer = new IndexWriter(directory, config);
                BufferedReader in = new BufferedReader(new InputStreamReader(System.in))) {
            DirectoryReader reader = DirectoryReader.open(writer);
            final QueryParser parser = new QueryParser("text", analyzer);
            final Document doc = new Document();
            final StringField idField = new StringField("id", "", Field.Store.YES);
            final TextField textField = new TextField("text", "", Field.Store.NO);
            doc.add(idField);
            doc.add(textField);
            String line;
            while ((line = in.readLine()) != null) {
                if (line.isEmpty()) {
                    continue;
                }
                final String[] f = line.split("\t", 3);
                switch (f[0]) {
                case "P":
                    idField.setStringValue(f[1]);
                    textField.setStringValue(f[2]);
                    writer.updateDocument(new Term("id", f[1]), doc);
                    break;
                case "D":
                    writer.deleteDocuments(new Term("id", f[1]));
                    break;
                case "C": {
                    final long start = System.nanoTime();
                    writer.commit();
                    final DirectoryReader newer = DirectoryReader.openIfChanged(reader, writer);
                    if (newer != null) {
                        reader.close();
                        reader = newer;
                    }
                    out.println("C\t" + (System.nanoTime() - start) / 1000 + "\t" + reader.leaves().size());
                    out.flush();
                    break;
                }
                case "Q": {
                    final long start = System.nanoTime();
                    final IndexSearcher searcher = new IndexSearcher(reader);
                    searcher.setQueryCache(null);
                    searcher.setSimilarity(new BM25Similarity(0.9f, 0.4f));
                    long result = 0;
                    try {
                        final Query query = parser.parse(f[2]);
                        switch (f[1]) {
                        case "COUNT":
                            result = searcher.count(query);
                            break;
                        case "TOP_10_COUNT":
                            result = searcher.search(query, new TopScoreDocCollectorManager(10, null, Integer.MAX_VALUE, false)).totalHits.value();
                            break;
                        default:
                            searcher.search(query, new TopScoreDocCollectorManager(10, null, 10, false));
                            result = 1;
                        }
                    } catch (org.apache.lucene.queryparser.classic.ParseException e) {
                        result = -1;
                    }
                    out.println("Q\t" + (System.nanoTime() - start) / 1000 + "\t" + result);
                    out.flush();
                    break;
                }
                default:
                    break;
                }
            }
            writer.commit();
            reader.close();
        }
    }
}
