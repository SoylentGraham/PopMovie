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

TVideoDeviceMeta GetDecoderMeta(const TVideoDecoderParams& Params)
{
	return TVideoDeviceMeta("file", Params.mFilename );
}

TMovieDecoder::TMovieDecoder(const TVideoDecoderParams& Params) :
	TVideoDevice	( GetDecoderMeta(Params) ),
	SoyWorkerThread	( Params.mFilename, SoyWorkerWaitMode::Wake )
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
	
	return GetDecoderMeta( mDecoder->mParams );
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
	
	//	send new frame
	OnNewFrame( Pixels, NextFrameTime );
	
	PixelBuffer->Unlock();
	
	return true;
}



TPopMovie::TPopMovie() :
	TJobHandler		( static_cast<TChannelManager&>(*this) ),
	TPopJobHandler	( static_cast<TJobHandler&>(*this) )
{
	AddJobHandler("exit", TParameterTraits(), *this, &TPopMovie::OnExit );
	
	TParameterTraits DecodeParameterTraits;
	DecodeParameterTraits.mAssumedKeys.PushBack("ref");
	DecodeParameterTraits.mAssumedKeys.PushBack("filename");
	AddJobHandler("decode", DecodeParameterTraits, *this, &TPopMovie::OnStartDecode );

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
	
	auto Ref = Job.mParams.GetParamAs<std::string>("ref");
	auto Filename = Job.mParams.GetParamAs<std::string>("filename");
	
	auto& Video = mVideos[Ref];
	if ( Video != nullptr )
	{
		std::stringstream Error;
		Error << "Video with ref " << Ref << " already exists";
		Reply.mParams.AddErrorParam( Error.str() );
		
		TChannel& Channel = JobAndChannel;
		Channel.OnJobCompleted( Reply );
		
		return;
	}

	TVideoDecoderParams DecoderParams( Filename, SoyPixelsFormat::RGBA );
	
	try
	{
		Video.reset( new TMovieDecoder( DecoderParams ) );

		std::stringstream Output;
		Output << "Created video " << Ref;
		Reply.mParams.AddDefaultParam( Output.str() );
	}
	catch ( std::exception& e )
	{
		Reply.mParams.AddErrorParam( std::string(e.what()) );
	}
	
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




