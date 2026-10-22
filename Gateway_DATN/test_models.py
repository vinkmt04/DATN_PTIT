import requests
import json
API_KEY = "AQ.Ab8RN6IdLF13NdQRBBf7Ge9dh9sFjg8VX3JxyUwLeFzdmUVELA"
url = f"https://generativelanguage.googleapis.com/v1beta/models?key={API_KEY}&pageSize=100"
response = requests.get(url)
models = response.json().get('models', [])
for m in models:
    name = m.get('name')
    methods = m.get('supportedGenerationMethods', [])
    if 'bidiGenerateContent' in methods:
        print(f"SUPPORTED MODEL: {name}")
