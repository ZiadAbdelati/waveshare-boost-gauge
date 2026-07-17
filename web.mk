.PHONY: web-assets
web-assets:
	python3 tools/embed_web.py web main/generated_web_assets.c main/generated_web_assets.h
