#!/bin/sh

set -e  # Exit on any error

echo "🔧 Preparing development workspace..."

# Set git commit template
git config --local commit.template .gitmessage

echo "✅ Development workspace is ready!"
