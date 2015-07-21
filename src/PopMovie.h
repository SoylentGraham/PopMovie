#pragma once
#include <SoyApp.h>
#include <TJob.h>
#include <TChannel.h>
#include <SoyVideoDevice.h>
#include <SoyOpenglContext.h>


class TVideoDecoder;
class TVideoDecoderParams;

class TPopMovie : public TJobHandler, public TPopJobHandler, public TChannelManager
{
public:
	TPopMovie();
	
	virtual bool	AddChannel(std::shared_ptr<TChannel> Channel) override;

	void			OnExit(TJobAndChannel& JobAndChannel);
	void			OnStartDecode(TJobAndChannel& JobAndChannel);
	
public:
	std::map<std::string,std::shared_ptr<TVideoDevice>>	mVideos;	//	videos we're processing and their id's
	Soy::Platform::TConsoleApp	mConsoleApp;
};


class TMovieDecoder : public TVideoDevice, public SoyWorkerThread
{
public:
	TMovieDecoder(const TVideoDecoderParams& Params);

	virtual TVideoDeviceMeta	GetMeta() const override;

	virtual bool				Iteration();
	
public:
	Opengl::TContext				mDummyContext;
	std::shared_ptr<TVideoDecoder>	mDecoder;
};

