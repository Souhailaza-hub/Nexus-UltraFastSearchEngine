"""Minimal command-line test interface for the Python/C++ bridge."""
import argparse
from engine_client import EngineClient


def main():
    parser = argparse.ArgumentParser(description="Index a directory and search names with NEXUS")
    parser.add_argument("root", help="Directory to index")
    parser.add_argument("query", nargs="?", default="", help="Prefix query")
    args = parser.parse_args()
    client = EngineClient()
    try:
        client.index_async(args.root).result()
        for record in client.search_async(args.query).result():
            print(record.path)
        print(client.engine.stats())
    finally:
        client.close()


if __name__ == "__main__":
    main()
