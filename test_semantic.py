import sqlite3
import sqlite_vec
from sentence_transformers import SentenceTransformer

db = sqlite3.connect("findx.db")
db.enable_load_extension(True)
sqlite_vec.load(db)
db.enable_load_extension(False)

model = SentenceTransformer("all-MiniLM-L6-v2")
query = "putting numbers in order from smallest to largest"
query_vec = model.encode(query).tolist()

rows = db.execute("""
    WITH knn_matches AS (
        SELECT chunk_id, distance
        FROM chunk_vectors
        WHERE embedding MATCH ?
        AND k = 5
    )
    SELECT chunks.doc_id, documents.path, knn_matches.distance
    FROM knn_matches
    JOIN chunks ON chunks.id = knn_matches.chunk_id
    JOIN documents ON documents.id = chunks.doc_id
    ORDER BY knn_matches.distance
""", (sqlite_vec.serialize_float32(query_vec),)).fetchall()

for row in rows:
    print(row)