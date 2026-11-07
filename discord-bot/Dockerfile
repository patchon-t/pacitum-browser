FROM python:3.12-slim
WORKDIR /bot
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY pacitum_bot.py .
# Keep state.json (the last posted release) in a volume: docker run -v pacitum-bot:/bot/data ...
ENV STATE_DIR=/bot/data
CMD ["python", "pacitum_bot.py"]
