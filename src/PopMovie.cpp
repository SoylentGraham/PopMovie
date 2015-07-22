#include "PopMovie.h"
#include <TParameters.h>
#include <SoyDebug.h>
#include <TProtocolCli.h>
#include <TProtocolHttp.h>
#include <SoyApp.h>
#include <PopMain.h>
#include <TJobRelay.h>
#include <SoyPixels.h>
#include <SoyString.h>
#include <TFeatureBinRing.h>
#include <SortArray.h>
#include <TChannelLiteral.h>
#include <TChannelFile.h>
#include <Build/PopMovieTextureOsxFramework.framework/Headers/AvfMovieDecoder.h>
#include <RemoteArray.h>



void TMovieDecoderContainer::GetDevices(ArrayBridge<TVideoDeviceMeta>& Metas)
{
	for ( int m=0;	m<mMovies.GetSize();	m++ )
	{
		Metas.PushBack( mMovies[m]->GetMeta() );
	}
}

std::shared_ptr<TVideoDevice> TMovieDecoderContainer::AllocDevice(const TVideoDeviceMeta& Meta,std::stringstream& Error)
{
	//	todo: look for existing
	
	TVideoDecoderParams DecoderParams( Meta.mName, SoyPixelsFormat::RGBA );
	try
	{
		std::shared_ptr<TMovieDecoder> Movie( new TMovieDecoder( DecoderParams, Meta.mSerial ) );
		mMovies.PushBack( Movie );
		return Movie;
	}
	catch ( std::exception& e )
	{
		Error << "Failed to create movie decoder: " << e.what();
		return nullptr;
	}
}

TVideoDeviceMeta GetDecoderMeta(const TVideoDecoderParams& Params,const std::string& Serial)
{
	TVideoDeviceMeta Meta( Serial, Params.mFilename );
	Meta.mVideo = true;
	Meta.mTimecode = true;
	//Meta.mConnected
	return Meta;
}

TMovieDecoder::TMovieDecoder(const TVideoDecoderParams& Params,const std::string& Serial) :
	TVideoDevice	( GetDecoderMeta(Params,Serial) ),
	SoyWorkerThread	( Params.mFilename, SoyWorkerWaitMode::Wake ),
	mSerial			( Serial )
{
	mDecoder = Platform::AllocDecoder( Params );
	mDecoder->StartMovie( mDummyContext );
	WakeOnEvent( mDecoder->mOnFrameDecoded );
	
	Start();
}
	
TVideoDeviceMeta TMovieDecoder::GetMeta() const
{
	if ( !mDecoder )
		return TVideoDeviceMeta();
	
	return GetDecoderMeta( mDecoder->mParams, mSerial );
}

bool TMovieDecoder::CanSleep()
{
	if ( !mDecoder )
		return true;
	
	auto NextFrameTime = mDecoder->GetNextPixelBufferTime();

	//	got a frame to read, don't sleep!
	if ( NextFrameTime.IsValid() )
		return false;

	return true;
}

bool TMovieDecoder::Iteration()
{
	if ( !mDecoder )
		return true;

	//	pop pixels
	auto NextFrameTime = mDecoder->GetNextPixelBufferTime();
	auto PixelBuffer = mDecoder->PopPixelBuffer( NextFrameTime );
	if ( !PixelBuffer )
		return true;
	
	SoyPixelsMetaFull PixelsMeta;
	auto* PixelsData = PixelBuffer->Lock( PixelsMeta );
	if ( !PixelsData )
		return true;
	
	auto PixelsDataSize = PixelsMeta.GetDataSize();
	auto PixelsArray = GetRemoteArray( PixelsData, PixelsDataSize );
	SoyPixelsDef<FixedRemoteArray<uint8>> Pixels( PixelsArray, PixelsMeta );

	static bool DoNewFrameLock = true;
	if ( DoNewFrameLock )
	{
		SoyPixelsImpl& NewFramePixels = LockNewFrame();
		NewFramePixels.Copy( Pixels );
		PixelBuffer->Unlock();
		UnlockNewFrame( NextFrameTime );
	}
	else
	{
		//	send new frame
		OnNewFrame( Pixels, NextFrameTime );
		PixelBuffer->Unlock();
	}
	
	return true;
}



TPopMovie::TPopMovie() :
	TJobHandler			( static_cast<TChannelManager&>(*this) ),
	TPopJobHandler		( static_cast<TJobHandler&>(*this) ),
	mSubcriberManager	( static_cast<TChannelManager&>(*this) )
{
	//	add videodecoder contaienr
	mMovies.reset( new TMovieDecoderContainer() );
	mVideoCapture.AddContainer( mMovies );

	AddJobHandler("exit", TParameterTraits(), *this, &TPopMovie::OnExit );
	
	TParameterTraits DecodeParameterTraits;
	DecodeParameterTraits.mAssumedKeys.PushBack("serial");
	DecodeParameterTraits.mAssumedKeys.PushBack("filename");
	AddJobHandler("decode", DecodeParameterTraits, *this, &TPopMovie::OnStartDecode );

	AddJobHandler("list", TParameterTraits(), *this, &TPopMovie::OnList );
	TParameterTraits GetFrameTraits;
	GetFrameTraits.mAssumedKeys.PushBack("serial");
	GetFrameTraits.mRequiredKeys.PushBack("serial");
	AddJobHandler("getframe", GetFrameTraits, *this, &TPopMovie::OnGetFrame );

	TParameterTraits SubscribeNewFrameTraits;
	SubscribeNewFrameTraits.mAssumedKeys.PushBack("serial");
	SubscribeNewFrameTraits.mDefaultParams.PushBack( std::make_tuple(std::string("command"),std::string("newframe")) );
	AddJobHandler("subscribenewframe", SubscribeNewFrameTraits, *this, &TPopMovie::SubscribeNewFrame );
}

bool TPopMovie::AddChannel(std::shared_ptr<TChannel> Channel)
{
	if ( !TChannelManager::AddChannel( Channel ) )
		return false;
	if ( !Channel )
		return false;
	TJobHandler::BindToChannel( *Channel );
	return true;
}


void TPopMovie::OnExit(TJobAndChannel& JobAndChannel)
{
	mConsoleApp.Exit();
	
	//	should probably still send a reply
	TJobReply Reply( JobAndChannel );
	Reply.mParams.AddDefaultParam(std::string("exiting..."));
	TChannel& Channel = JobAndChannel;
	Channel.OnJobCompleted( Reply );
}



void TPopMovie::OnStartDecode(TJobAndChannel& JobAndChannel)
{
	auto Job = JobAndChannel.GetJob();
	TJobReply Reply( JobAndChannel );
	
	auto Serial = Job.mParams.GetParamAs<std::string>("serial");
	auto Filename = Job.mParams.GetParamAs<std::string>("filename");
	
	TVideoDeviceMeta Meta( Serial, Filename );
	std::stringstream Error;
	auto Device = mMovies->AllocDevice( Meta, Error );
	

	if ( !Error.str().empty() )
		Reply.mParams.AddErrorParam( Error.str() );

	if ( Device )
		Reply.mParams.AddDefaultParam( Serial );
	
	TChannel& Channel = JobAndChannel;
	Channel.OnJobCompleted( Reply );
}


bool TPopMovie::OnNewFrameCallback(TEventSubscriptionManager& SubscriptionManager,TJobChannelMeta Client,TVideoDevice& Device)
{
	TJob OutputJob;
	auto& Reply = OutputJob;
	
	std::stringstream Error;
	//	grab pixels
	auto& LastFrame = Device.GetLastFrame(Error);
	if ( LastFrame.IsValid() )
	{
		auto& MemFile = LastFrame.mPixels.mMemFileArray;
		TYPE_MemFile MemFileData( MemFile );
		Reply.mParams.AddDefaultParam( MemFileData );
	}
	
	//	add error if present (last frame could be out of date)
	if ( !Error.str().empty() )
		Reply.mParams.AddErrorParam( Error.str() );
	
	//	find channel, send to Client
	//	std::Debug << "Got event callback to send to " << Client << std::endl;
	
	if ( !SubscriptionManager.SendSubscriptionJob( Reply, Client ) )
		return false;
	
	return true;
}


void TPopMovie::OnGetFrame(TJobAndChannel& JobAndChannel)
{
	const TJob& Job = JobAndChannel;
	TJobReply Reply( JobAndChannel );
	
	auto Serial = Job.mParams.GetParamAs<std::string>("serial");
	auto AsMemFile = Job.mParams.GetParamAsWithDefault<bool>("memfile",true);
	
	std::Debug << Job.mParams << std::endl;
	
	std::stringstream Error;
	auto Device = mVideoCapture.GetDevice( Serial, Error );
	
	if ( !Device )
	{
		std::stringstream ReplyError;
		ReplyError << "Device " << Serial << " not found " << Error.str();
		Reply.mParams.AddErrorParam( ReplyError.str() );
		TChannel& Channel = JobAndChannel;
		Channel.OnJobCompleted( Reply );
		return;
	}
	
	//	grab pixels
	auto& LastFrame = Device->GetLastFrame( Error );
	if ( LastFrame.IsValid() )
	{
		if ( AsMemFile )
		{
			TYPE_MemFile MemFile( LastFrame.mPixels.mMemFileArray );
			TJobFormat Format;
			Format.PushFirstContainer<SoyPixels>();
			Format.PushFirstContainer<TYPE_MemFile>();
			Reply.mParams.AddDefaultParam( MemFile, Format );
		}
		else
		{
			SoyPixels Pixels;
			Pixels.Copy( LastFrame.mPixels );
			Reply.mParams.AddDefaultParam( Pixels );
		}
	}
	
	//	add error if present (last frame could be out of date)
	if ( !Error.str().empty() )
		Reply.mParams.AddErrorParam( Error.str() );
	
	//	add other stats
	auto FrameRate = Device->GetFps();
	auto FrameMs = Device->GetFrameMs();
	Reply.mParams.AddParam("fps", FrameRate);
	Reply.mParams.AddParam("framems", FrameMs );
	Reply.mParams.AddParam("serial", Device->GetMeta().mSerial );
	
	TChannel& Channel = JobAndChannel;
	Channel.OnJobCompleted( Reply );
	
}

//	copied from TPopCapture::OnListDevices
void TPopMovie::OnList(TJobAndChannel& JobAndChannel)
{
	TJobReply Reply( JobAndChannel );
	
	Array<TVideoDeviceMeta> Metas;
	mVideoCapture.GetDevices( GetArrayBridge(Metas) );
	
	std::stringstream MetasString;
	for ( int i=0;	i<Metas.GetSize();	i++ )
	{
		auto& Meta = Metas[i];
		if ( i > 0 )
			MetasString << ",";
		
		MetasString << Meta;
	}
	
	if ( !MetasString.str().empty() )
		Reply.mParams.AddDefaultParam( MetasString.str() );
	else
		Reply.mParams.AddErrorParam("No devices found");
	
	TChannel& Channel = JobAndChannel;
	Channel.OnJobCompleted( Reply );
}


void TPopMovie::SubscribeNewFrame(TJobAndChannel& JobAndChannel)
{
	const TJob& Job = JobAndChannel;
	TJobReply Reply( JobAndChannel );
	
	std::stringstream Error;
	
	//	get device
	auto Serial = Job.mParams.GetParamAs<std::string>("serial");
	auto Device = mVideoCapture.GetDevice( Serial, Error );
	if ( !Device )
	{
		std::stringstream ReplyError;
		ReplyError << "Device " << Serial << " not found " << Error.str();
		Reply.mParams.AddErrorParam( ReplyError.str() );
		TChannel& Channel = JobAndChannel;
		Channel.OnJobCompleted( Reply );
		return;
	}
	
	//	create new subscription for it
	//	gr: determine if this already exists!
	auto EventName = Job.mParams.GetParamAs<std::string>("command");
	auto Event = mSubcriberManager.AddEvent( Device->mOnNewFrame, EventName, Error );
	if ( !Event )
	{
		std::stringstream ReplyError;
		ReplyError << "Failed to create new event " << EventName << ". " << Error.str();
		Reply.mParams.AddErrorParam( ReplyError.str() );
		TChannel& Channel = JobAndChannel;
		Channel.OnJobCompleted( Reply );
		return;
	}
	
	//	make a lambda to recieve the event
	auto Client = Job.mChannelMeta;
	TEventSubscriptionCallback<TVideoDevice> ListenerCallback = [this,Client](TEventSubscriptionManager& SubscriptionManager,TVideoDevice& Value)
	{
		return this->OnNewFrameCallback( SubscriptionManager, Client, Value );
	};
	
	//	subscribe this caller
	if ( !Event->AddSubscriber( Job.mChannelMeta, ListenerCallback, Error ) )
	{
		std::stringstream ReplyError;
		ReplyError << "Failed to add subscriber to event " << EventName << ". " << Error.str();
		Reply.mParams.AddErrorParam( ReplyError.str() );
		TChannel& Channel = JobAndChannel;
		Channel.OnJobCompleted( Reply );
		return;
	}
	
	
	std::stringstream ReplyString;
	ReplyString << "OK subscribed to " << EventName;
	Reply.mParams.AddDefaultParam( ReplyString.str() );
	if ( !Error.str().empty() )
		Reply.mParams.AddErrorParam( Error.str() );
	Reply.mParams.AddParam("eventcommand", EventName);
	
	TChannel& Channel = JobAndChannel;
	Channel.OnJobCompleted( Reply );
}


//	keep alive after PopMain()
#if defined(TARGET_OSX_BUNDLE)
std::shared_ptr<TPopMovie> gApp;
#endif


TPopAppError::Type PopMain(TJobParams& Params)
{
	std::cout << Params << std::endl;
	
	gApp.reset( new TPopMovie );
	auto& App = *gApp;

	auto CommandLineChannel = std::shared_ptr<TChan<TChannelLiteral,TProtocolCli>>( new TChan<TChannelLiteral,TProtocolCli>( SoyRef("cmdline") ) );
	
	//	create stdio channel for commandline output
	auto StdioChannel = CreateChannelFromInputString("std:", SoyRef("stdio") );
	auto HttpChannel = CreateChannelFromInputString("http:8080-8090", SoyRef("http") );
	
	
	App.AddChannel( CommandLineChannel );
	App.AddChannel( StdioChannel );
	App.AddChannel( HttpChannel );

	
	
	
	
	//	bootup commands via a channel
	std::shared_ptr<TChannel> BootupChannel( new TChan<TChannelFileRead,TProtocolCli>( SoyRef("Bootup"), "bootup.txt" ) );
	/*
	//	display reply to stdout
	//	when the commandline SENDs a command (a reply), send it to stdout
	auto RelayFunc = [](TJobAndChannel& JobAndChannel)
	{
		std::Debug << JobAndChannel.GetJob().mParams << std::endl;
	};
	//BootupChannel->mOnJobRecieved.AddListener( RelayFunc );
	BootupChannel->mOnJobSent.AddListener( RelayFunc );
	BootupChannel->mOnJobLost.AddListener( RelayFunc );
	*/
	App.AddChannel( BootupChannel );
	


#if !defined(TARGET_OSX_BUNDLE)
	//	run
	App.mConsoleApp.WaitForExit();
#endif

	return TPopAppError::Success;
}




