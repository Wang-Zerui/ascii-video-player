import tempfile
import video_engine as ve

def play(filename, strategy=None, output=None, play_audio=False):
    engine = ve.VideoEngine()
    engine.load_video_from_file(filename)
    if play_audio:
        import ffmpeg
        temp_dir = tempfile.gettempdir()
        temp_file_path = temp_dir + "/temp-audiofile-for-vta.wav"
        stream = ffmpeg.input(filename)
        stream = ffmpeg.output(stream, temp_file_path)
        stream = ffmpeg.overwrite_output(stream)
        ffmpeg.run(stream)
        engine.with_audio = True
    if strategy is not None:
        engine.set_strategy(strategy)
    engine.play(output)
