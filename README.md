
# rtsp_py 

完整的python 的rtsp server和client的实现，服务端rtsp server, 服务端收到play

	self.clientInfo["rtpSocket"] = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
	开始发送数据
	self.clientInfo['worker'] = threading.Thread(target=self.sendRtp)

	这里发送的数据是movie.Mjpeg(没帧都是一个jpeg) data发过去， client端存为文件，然后用tk直接显示jpeg
	filename = self.writeFrame(pkt.getPayload()) //写入文件
        self.updateMovie(filename, pkt.timestamp()) //更新tk
	
	def updateMovie:
		current_frame = ImageTk.PhotoImage(Image.open(imageFile))
        	self.label.configure(image=current_frame)
	客户端就播放了

# BXC_RtspServer_study_CPP

基本就是server端，建立rtspserver和rtpserver, 然后读取h264, 或者acc 在play的时候send给client
