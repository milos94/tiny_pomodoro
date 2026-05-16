#!/usr/bin/env python3
"""Quick sanity-check for the reveal.js presentation."""
from html.parser import HTMLParser


class Checker(HTMLParser):
    def __init__(self):
        super().__init__()
        self.sections = 0
        self.images = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "section":
            self.sections += 1
        if tag == "img":
            self.images.append(attrs.get("src", ""))


c = Checker()
with open("index.html") as f:
    c.feed(f.read())

print(f"Sections (slides): {c.sections}")
print(f"Images referenced: {len(c.images)}")
for img in c.images:
    print(f"  {img}")
print("HTML parses OK")
