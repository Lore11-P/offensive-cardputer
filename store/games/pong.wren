// name: Pong
// desc: Tu contro la CPU, primo a 5 punti.
// Pong per ORBIT OS - 240x135, ENTER per giocare di nuovo.
var bg = Game.rgb(8, 10, 18)
var panel = Game.rgb(18, 22, 34)
var fg = Game.rgb(225, 238, 255)
var acc = Game.rgb(90, 220, 255)
var hot = Game.rgb(255, 190, 60)
var dim = Game.rgb(110, 125, 150)

var top = 15
var paddleH = 26
var playerX = 8
var cpuX = 226
var state = 0
var py = 50
var cy = 55
var ballX = 118
var ballY = 70
var vx = 2.4
var vy = 2.2
var playerScore = 0
var cpuScore = 0
var serve = 0
var won = false

var serveBall = Fn.new { |dir|
  ballX = 118
  ballY = Game.height / 2 - 2
  vy = 2.2
  vx = 2.4
  if (dir < 0) {
    vx = -2.4
  }
  serve = Game.millis + 500
}

var resetMatch = Fn.new {
  playerScore = 0
  cpuScore = 0
  py = 50
  cy = 55
  serveBall.call(1)
  state = 1
}

var drawHeader = Fn.new {
  Game.rect(0, 0, Game.width, top, panel)
  Game.text(6, 3, "PONG", acc, 0)
  var s = "TU %(playerScore) - %(cpuScore) CPU"
  Game.text((Game.width - (s.count * 6 - 1)) / 2 + 16, 3, s, fg, 0)
}

var drawCourt = Fn.new {
  Game.rect(0, top, Game.width, Game.height - top, bg)
  for (i in 0...10) {
    Game.rect(118, top + 4 + i * 12, 2, 6, dim)
  }
  Game.rect(playerX, py, 6, paddleH, acc)
  Game.rect(cpuX, cy, 6, paddleH, hot)
  if (Game.millis >= serve || Game.millis % 240 < 120) {
    Game.rect(ballX, ballY, 5, 5, fg)
  }
  if (state == 2) {
    Game.rect(34, 44, 172, 52, panel)
    var title = "HAI VINTO!"
    var color = acc
    if (!won) {
      title = "CPU VINCE"
      color = hot
    }
    Game.text((Game.width - (title.count * 12 - 2)) / 2, 49, title, color, 4)
    Game.text(52, 70, "ENTER nuova partita", fg, 0)
    Game.text(78, 82, "ESC esci", dim, 0)
  }
}

var drawTitle = Fn.new {
  Game.rect(0, 0, Game.width, Game.height, bg)
  Game.text(94, 24, "PONG", acc, 4)
  Game.rect(58, 56, 5, 22, acc)
  Game.rect(177, 52, 5, 22, hot)
  Game.rect(118, 62, 5, 5, fg)
  Game.text(70, 86, "TU vs CPU - primo a 5", fg, 0)
  Game.text(60, 99, "SU/GIU: muovi", hot, 0)
  Game.text(45, 111, "ENTER: gioca    ESC: esci", dim, 0)
}

while (true) {
  Game.frame()
  if (state == 0) {
    drawTitle.call()
    if (Game.select) {
      resetMatch.call()
    }
  } else {
    if (state == 1) {
      if (Game.up) {
        py -= 4
      }
      if (Game.down) {
        py += 4
      }
      if (py < top) {
        py = top
      }
      if (py > Game.height - paddleH) {
        py = Game.height - paddleH
      }
      if (vx > 0) {
        if (ballY + 2 < cy + 13) {
          cy -= 3
        } else if (ballY + 2 > cy + 13) {
          cy += 3
        }
      } else {
        if (cy + 13 < Game.height / 2) {
          cy += 2
        } else if (cy + 13 > Game.height / 2) {
          cy -= 2
        }
      }
      if (cy < top) {
        cy = top
      }
      if (cy > Game.height - paddleH) {
        cy = Game.height - paddleH
      }
      if (Game.millis >= serve) {
        ballX += vx
        ballY += vy
        if (ballY < top) {
          ballY = top
          vy = -vy
        }
        if (ballY > Game.height - 5) {
          ballY = Game.height - 5
          vy = -vy
        }
        if (vx < 0 && ballX <= playerX + 6 && ballX >= playerX - 4 &&
            ballY + 5 >= py && ballY <= py + paddleH) {
          ballX = playerX + 6
          vx = -vx * 1.03
          vy = ((ballY + 2) - (py + 13)) / 5
          if (vy > 3.2) {
            vy = 3.2
          }
          if (vy < -3.2) {
            vy = -3.2
          }
          Game.beep(660, 15)
        }
        if (vx > 0 && ballX + 5 >= cpuX && ballX <= cpuX + 6 &&
            ballY + 5 >= cy && ballY <= cy + paddleH) {
          ballX = cpuX - 5
          vx = -vx * 1.03
          vy = ((ballY + 2) - (cy + 13)) / 5
          if (vy > 3.2) {
            vy = 3.2
          }
          if (vy < -3.2) {
            vy = -3.2
          }
          Game.beep(440, 15)
        }
        if (ballX + 5 < 0) {
          cpuScore += 1
          Game.beep(180, 120)
          if (cpuScore >= 5) {
            won = false
            state = 2
          } else {
            serveBall.call(-1)
          }
        } else if (ballX > Game.width) {
          playerScore += 1
          Game.beep(980, 120)
          if (playerScore >= 5) {
            won = true
            state = 2
          } else {
            serveBall.call(1)
          }
        }
      }
    } else if (Game.select) {
      resetMatch.call()
    }
    drawHeader.call()
    drawCourt.call()
  }
  Game.present()
}
