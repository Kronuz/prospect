// strata's soak op stream (bench/soak/soak.py) through Tantivy: upserts and deletes by an
// indexed id, commits (with the reader reloaded), and queries.
use std::env;
use std::io::{BufRead, Write};
use std::path::Path;
use std::time::Instant;

use tantivy::collector::{Count, TopDocs};
use tantivy::query::QueryParser;
use tantivy::schema::{Schema, STORED, STRING, TEXT};
use tantivy::tokenizer::TokenizerManager;
use tantivy::{doc, Index, IndexWriter, ReloadPolicy, TantivyDocument, Term};

fn main() -> tantivy::Result<()> {
    let args: Vec<String> = env::args().collect();
    let dir = Path::new(&args[1]);
    std::fs::create_dir_all(dir)?;
    let mut builder = Schema::builder();
    let id = builder.add_text_field("id", STRING | STORED);
    let text = builder.add_text_field("text", TEXT);
    let schema = builder.build();
    let index = Index::create_in_dir(dir, schema.clone())?;
    let mut writer: IndexWriter<TantivyDocument> = index.writer_with_num_threads(1, 500_000_000)?;
    let reader = index.reader_builder().reload_policy(ReloadPolicy::Manual).try_into()?;
    let parser = QueryParser::new(schema, vec![text], TokenizerManager::default());
    let stdout = std::io::stdout();
    let mut out = stdout.lock();
    for line in std::io::stdin().lock().lines() {
        let line = line?;
        let fields: Vec<&str> = line.splitn(3, '\t').collect();
        match fields[0] {
            "P" => {
                writer.delete_term(Term::from_field_text(id, fields[1]));
                writer.add_document(doc!(id => fields[1], text => fields[2]))?;
            }
            "D" => {
                writer.delete_term(Term::from_field_text(id, fields[1]));
            }
            "C" => {
                let start = Instant::now();
                writer.commit()?;
                reader.reload()?;
                let us = start.elapsed().as_micros();
                writeln!(out, "C\t{}\t{}", us, reader.searcher().segment_readers().len())?;
                out.flush()?;
            }
            "Q" => {
                let start = Instant::now();
                let searcher = reader.searcher();
                let query = parser.parse_query(fields[2])?;
                let result = match fields[1] {
                    "COUNT" => query.count(&searcher)?,
                    "TOP_10_COUNT" => searcher.search(&query, &(TopDocs::with_limit(10).order_by_score(), Count))?.1,
                    _ => {
                        let _top = searcher.search(&query, &TopDocs::with_limit(10).order_by_score())?;
                        1
                    }
                };
                writeln!(out, "Q\t{}\t{}", start.elapsed().as_micros(), result)?;
                out.flush()?;
            }
            _ => {}
        }
    }
    writer.commit()?;
    writer.wait_merging_threads()?;
    Ok(())
}
