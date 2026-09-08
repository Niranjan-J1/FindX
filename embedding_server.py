import socket
from sentence_transformers import SentenceTransformer

HOST = "127.0.0.1"
PORT = 8765
MODEL_NAME = "all-MiniLM-L6-v2"

def main():
    print("Loading embedding model...")
    model = SentenceTransformer(MODEL_NAME)
    print("Model loaded.")

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, PORT))
    server.listen(1)
    print(f"Listening on {HOST}:{PORT}")

    while True:
        conn, addr = server.accept()
        with conn:
            data = b""
            while not data.endswith(b"\n"):
                chunk = conn.recv(4096)
                if not chunk:
                    break
                data += chunk

            query = data.decode("utf-8").strip()
            if not query:
                continue

            print(f"Embedding: {query}")
            embedding = model.encode(query).tolist()
            response = ",".join(str(x) for x in embedding) + "\n"
            conn.sendall(response.encode("utf-8"))

if __name__ == "__main__":
    main()