#pragma once
#include <SoyApp.h>
#include <TJob.h>
#include <TChannel.h>
#include <SoyVideoDevice.h>
#include <SoyOpenglContext.h>
#include <TJobEventSubscriber.h>


class TVideoDecoder;
class TVideoDecoderParams;
class TMovieDecoderContainer;
class TMovieDecoder;


class TPopMovie : public TJobHandler, public TPopJobHandler, public TChannelManager
{
public:
	TPopMovie();
	
	virtual bool	AddChannel(std::shared_ptr<TChannel> Channel) override;

	void			OnExit(TJobAndChannel& JobAndChannel);

	void			OnStartDecode(TJobAndChannel& JobAndChannel);
	void			OnList(TJobAndChannel& JobAndChannel);
	
	void			OnGetFrame(TJobAndChannel& JobAndChannel);
	
	void			SubscribeNewFrame(TJobAndChannel& JobAndChannel);
	bool			OnNewFrameCallback(TEventSubscriptionManager& SubscriptionManager,TJobChannelMeta Client,TVideoDevice& Device);

	
public:
	Soy::Platform::TConsoleApp	mConsoleApp;
	std::shared_ptr<TMovieDecoderContainer>	mMovies;
	SoyVideoCapture		mVideoCapture;
	TSubscriberManager	mSubcriberManager;
};


class TMovieDecoderContainer : public SoyVideoContainer
{
public:
	virtual void							GetDevices(ArrayBridge<TVideoDeviceMeta>& Metas) override;
	virtual std::shared_ptr<TVideoDevice>	AllocDevice(const TVideoDeviceMeta& Meta,std::stringstream& Error) override;

	Array<std::shared_ptr<TMovieDecoder>>	mMovies;
};

class TMovieDecoder : public TVideoDevice, public SoyWorkerThread
{
public:
	TMovieDecoder(const TVideoDecoderParams& Params,const std::string& Serial);

	virtual TVideoDeviceMeta	GetMeta() const override;

	virtual bool				Iteration() override;
	virtual bool				CanSleep() override;
	
	
public:
	std::string						mSerial;
	Opengl::TContext				mDummyContext;
	std::shared_ptr<TVideoDecoder>	mDecoder;
};

