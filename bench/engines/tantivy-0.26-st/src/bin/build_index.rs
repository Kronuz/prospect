use futures::executor::block_on;
use std::env;
use std::io::BufRead;
use std::path::Path;
use tantivy::schema::{Schema, STORED, STRING, TEXT, FAST, Value};
use tantivy::Term;
use tantivy::{Index, IndexWriter, TantivyDocument};

fn main() {
    let args: Vec<String> = env::args().collect();
    main_inner(Path::new(&args[1])).unwrap();
}

fn create_schema() -> Schema {
    let mut schema_builder = Schema::builder();
    // KEYED=1: each document is upserted by its id (T4's "keys in every engine").
    let keyed = std::env::var("KEYED").map(|v| v == "1").unwrap_or(false);
    schema_builder.add_text_field("id", if keyed { STRING | STORED } else { STORED.into() });
    schema_builder.add_text_field("text", TEXT);
    schema_builder.add_u64_field("sort_field", FAST);
    schema_builder.build()
}

fn main_inner(output_dir: &Path) -> tantivy::Result<()> {
    env_logger::init();

    let schema = create_schema();
    let keyed = std::env::var("KEYED").map(|v| v == "1").unwrap_or(false);
    let index = Index::create_in_dir(output_dir, schema.clone()).expect("failed to create index");

    let mut i = 0;
    {
        let mut index_writer = index
            // TANTIVY_HEAP_MB sets the indexing heap, calibrated to equal peak memory.
            .writer_with_num_threads(1, std::env::var("TANTIVY_HEAP_MB").ok().and_then(|v| v.parse::<usize>().ok()).map_or(2_000_000_000, |mb| mb << 20))
            .expect("failed to create index writer");
        let stdin = std::io::stdin();

        for line in stdin.lock().lines() {
            let line = line?;
            if line.trim().is_empty() {
                continue;
            }
            i += 1;
            if i % 100_000 == 0 {
                println!("{}", i);
            }
            let doc = TantivyDocument::parse_json(&schema, &line)?;
            if keyed {
                let id_field = schema.get_field("id").unwrap();
                if let Some(id) = doc.get_first(id_field).and_then(|v| v.as_str()) {
                    index_writer.delete_term(Term::from_field_text(id_field, id));
                }
            }
            index_writer.add_document(doc).unwrap();
        }

        index_writer.commit()?;
        index_writer.wait_merging_threads()?;
    }
    // NO_MERGE=1 keeps the segments the merge policy left (Phase 3's "before merging" rows).
    if std::env::var("NO_MERGE").map(|v| v == "1").unwrap_or(false) {
        return Ok(());
    }
    let segment_ids = index.searchable_segment_ids()?;
    let mut index_writer: IndexWriter<TantivyDocument> = index
        .writer(1_500_000_000)
        .expect("failed to create index writer");
    block_on(index_writer.merge(&segment_ids))?;
    block_on(index_writer.garbage_collect_files())?;
    Ok(())
}
