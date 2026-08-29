import sqlite3
import sqlite_vec
from sentence_transformers import SentenceTransformer

DB_PATH = "findx.db"
MODEL_NAME = "all-MiniLM-L6-v2"
EMBEDDING_DIM = 384

def main():
    db = sqlite3.connect(DB_PATH)

    # load the sqlite-vec extension into this connection
    db.enable_load_extension(True)
    sqlite_vec.load(db)
    db.enable_load_extension(False)

    db.execute(f"""
        CREATE VIRTUAL TABLE IF NOT EXISTS chunk_vectors USING vec0(
            chunk_id INTEGER PRIMARY KEY,
            embedding FLOAT[{EMBEDDING_DIM}]
        );
    """)
    db.commit()

    # only embed chunks that don't already have a vector — same incremental spirit as v0.6
    rows = db.execute("""
        SELECT id, text FROM chunks
        WHERE id NOT IN (SELECT chunk_id FROM chunk_vectors)
    """).fetchall()

    if not rows:
        print("No new chunks to embed.")
        return

    print(f"Embedding {len(rows)} chunk(s)...")

    chunk_ids = [r[0] for r in rows]
    texts = [r[1] for r in rows]

    model = SentenceTransformer(MODEL_NAME)
    embeddings = model.encode(texts, show_progress_bar=True)

    for chunk_id, embedding in zip(chunk_ids, embeddings):
        db.execute(
            "INSERT INTO chunk_vectors (chunk_id, embedding) VALUES (?, ?)",
            (chunk_id, sqlite_vec.serialize_float32(embedding.tolist())),
        )

    db.commit()
    db.close()
    print(f"Done. {len(rows)} chunk(s) embedded.")

if __name__ == "__main__":
    main()