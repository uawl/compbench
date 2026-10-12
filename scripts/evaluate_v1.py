#!/usr/bin/env python3
"""Compatibility entry point for evaluating res/v1 submissions."""
import sys
from evaluate import main

if __name__ == "__main__":
    sys.exit(main("v1"))
