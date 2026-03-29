#!/usr/bin/env python3
import os

print("Content-Type: text/html; charset=utf-8")
print()
print("<!DOCTYPE html><html><head><title>CGI</title></head><body>")
print("<h1>Hola desde Python CGI</h1>")
print("<p>QUERY_STRING =", os.environ.get("QUERY_STRING", ""), "</p>")
print("<p>SCRIPT_NAME =", os.environ.get("SCRIPT_NAME", ""), "</p>")
print("</body></html>")
