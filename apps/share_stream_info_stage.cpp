
#include <libcamera/stream.h>
#include "core/rpicam_app.hpp"
#include "post_processing_stages/post_processing_stage.hpp"


//START NEEDED FOR IPC COMMUNCATION
#include <sys/ipc.h>
#include <sys/shm.h>
// #include <sys/types.h> ENABLE IF NEEDED
// END NEEDED FOR IPC COMMUNCATION


// START NEEDED FOR LOGGING
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

// END NEEDED FOR LOGGING



#define PROJECT_ID 0x4341494D // ASCII for "CAIM"
#define NAME "share_stream_info"


using Stream = libcamera::Stream;


struct SharedStreamData {
	SharedStreamData() : procid(-1) {}
	StreamInfo stream_info;
	int procid;
	int fd;
	int span_size;
	void resetStreamData() {
		procid = getpid();
		fd=-1;
		stream_info.width = 0;
		stream_info.height = 0;
		stream_info.stride = 0;
		stream_info.pixel_format = {};
		stream_info.colour_space.reset();
		span_size = -1;
	}
};



class shareStreamInfo : public PostProcessingStage
{
public:
	shareStreamInfo(RPiCamApp *app) ;
	virtual ~shareStreamInfo() override;
	char const *Name() const override;
	void Read(boost::property_tree::ptree const &params) override {}
	void Configure() override;
	bool Process(CompletedRequestPtr &completed_request) override;

private:
	Stream *stream_;
	std::shared_ptr<spdlog::logger> console;
	SharedStreamData* shared_data;
	int segment_id;
	key_t segment_key;
};


char const *shareStreamInfo::Name() const
{
	return NAME;
}

shareStreamInfo::shareStreamInfo(RPiCamApp *app) :PostProcessingStage(app) {

}

void shareStreamInfo::Configure()
{
	
	console = spdlog::stdout_color_mt("share_stream_info");
	console->info("share_stream_info is running (PID: {})", getpid()); // <-- 
	fprintf(stderr, "share_stream_info Configure() reached (PID: %d)\n", getpid());
	fflush(stderr);
	// Generate a unique key for the shared memory segment
    segment_key = ftok("/tmp", PROJECT_ID);
    console->info("sharedContextStage: ftok returned key 0x{:08X}", segment_key);
	segment_id = shmget(segment_key, sizeof(SharedStreamData), IPC_CREAT | S_IRUSR | S_IWUSR);
    console->info("sharedContextStage: sending Buffer of size {}", sizeof(SharedStreamData)	);
	if (segment_id == -1) {
        console->error("shmget failed with key \"0x{:08X}\", SharedStreamData size: {}  Error: \"{}\"",
            segment_key, sizeof(SharedStreamData), strerror(errno));
    }
	// Attach the shared memory segment
    shared_data = (SharedStreamData*)shmat(segment_id, NULL, 0); 
	// ADD ERROR HANDLING
	if (shared_data == (SharedStreamData*)-1) {
		console->error("Attaching the shared memory segment failed with key \"0x{:08X}\", Error: \"{}\"",
			segment_key, strerror(errno));
	}
	shared_data->resetStreamData();
	stream_ = app_->GetMainStream();
	shared_data->stream_info = app_->GetStreamInfo(stream_);



}

bool shareStreamInfo::Process(CompletedRequestPtr &completed_request)
{
	// console->info("updating buffer....."); // <-- 
	shared_data->procid = getpid();
	shared_data->fd = completed_request->buffers[app_->GetMainStream()]->planes()[0].fd.get();

	// BufferWriteSync w(app_, completed_request->buffers[stream_]);
	// libcamera::Span<uint8_t> span = w.Get()[0].size();                  ALTERNATIVE WAY TO GET SPAN SIZE

	shared_data->span_size = completed_request->buffers[stream_]->planes()[0].length;


	return false;
}

shareStreamInfo::~shareStreamInfo()
{
	shmdt(shared_data);
    shmctl(segment_id, IPC_RMID, NULL);
}

static PostProcessingStage *Create(RPiCamApp *app)
{
	return new shareStreamInfo(app);
}

static RegisterStage reg(NAME, &Create);
